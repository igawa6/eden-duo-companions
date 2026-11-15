// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ms_art.h"

#include "chained_echoes_unity.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <list>
#include <set>
#include <utility>

namespace ms {

namespace {

constexpr std::string_view DataPath = "Data/data.unity3d";
constexpr std::string_view IndexPath = "file:pictures.idx";
constexpr std::size_t MaxCachedTextures = 3; // up to 16 MiB each (2048x2048 RGBA)
constexpr std::string_view HostFontName = "regular";

std::vector<std::string_view> Fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
            ++i;
        const std::size_t b = i;
        while (i < line.size() && line[i] != ' ')
            ++i;
        if (b < i)
            out.push_back(line.substr(b, i - b));
    }
    return out;
}

template <typename T>
bool Num(std::string_view s, T& v) {
    if constexpr (std::is_floating_point_v<T>) {
        try {
            v = static_cast<T>(std::stod(std::string{s}));
            return true;
        } catch (...) {
            return false;
        }
    } else {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
        return r.ec == std::errc{} && r.ptr == s.data() + s.size();
    }
}

} // namespace

bool PictureIndex::Parse(std::string_view text) {
    bool version = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        const auto f = Fields(text.substr(pos, end - pos));
        pos = end + 1;
        if (f.empty())
            continue;
        const char kind = f[0].size() == 1 ? f[0][0] : '?';
        bool ok = true;
        switch (kind) {
        case 'M': {
            int v{};
            ok = f.size() == 2 && Num(f[1], v) && v == 1;
            version = ok;
            break;
        }
        case 'N': {
            std::size_t i{};
            ok = f.size() == 3 && Num(f[1], i) && i == nodes.size();
            if (ok)
                nodes.emplace_back(f[2]);
            break;
        }
        case 'Q': {
            PictureIndex::Text t;
            ok = f.size() == 5 && Num(f[2], t.node) && Num(f[3], t.offset) &&
                 Num(f[4], t.bytes) && t.bytes > 0 && t.bytes <= 200;
            if (ok)
                strings[std::string{f[1]}] = t;
            break;
        }
        case 'T': {
            int id{};
            Texture t;
            ok = f.size() == 8 && Num(f[1], id) && Num(f[2], t.node) && Num(f[3], t.offset) &&
                 Num(f[4], t.bytes) && Num(f[5], t.width) && Num(f[6], t.height) && Num(f[7], t.format);
            if (ok)
                textures[id] = t;
            break;
        }
        case 'S': {
            // a sprite name may contain spaces ("icon_skirmish 1"): the six numbers are last
            Sprite s;
            int rot{};
            const std::size_t n = f.size();
            ok = n >= 8 && Num(f[n - 6], s.tex) && Num(f[n - 5], s.x) && Num(f[n - 4], s.y) &&
                 Num(f[n - 3], s.w) && Num(f[n - 2], s.h) && Num(f[n - 1], rot);
            s.turned = rot != 0;
            if (ok) {
                std::string name{f[1]};
                for (std::size_t i = 2; i + 6 < n; ++i)
                    name += " " + std::string{f[i]};
                sprites.emplace(std::move(name), s);
            }
            break;
        }
        case 'L': {
            auto& fo = fonts[std::string{f.size() > 1 ? f[1] : ""}];
            ok = f.size() == 4 && Num(f[2], fo.line_height) && Num(f[3], fo.tex);
            break;
        }
        case 'G': {
            std::uint32_t cp{};
            Glyph g;
            int left{}, top{}, bottom{}, rot{};
            ok = f.size() == 12 && Num(f[2], cp) && Num(f[3], g.x) && Num(f[4], g.y) && Num(f[5], g.w) &&
                 Num(f[6], g.h) && Num(f[7], left) && Num(f[8], top) && Num(f[9], bottom) &&
                 Num(f[10], g.advance) && Num(f[11], rot);
            if (ok) {
                g.turned = rot != 0;
                g.bearing_x = left;
                g.bearing_y = top;   // font units, y up; made baseline-relative in BuildFont
                g.bottom = bottom; // g.w / g.h stay the stored (atlas) size
                fonts[std::string{f[1]}].glyphs[cp] = g;
            }
            break;
        }
        case 'A': {
            // A <set> <fps> <wrap> <loop start> <frame> <frame> ...
            Clip clip;
            ok = f.size() >= 6 && Num(f[2], clip.fps) && clip.fps > 0 && clip.fps <= 120 &&
                 Num(f[3], clip.wrap) && Num(f[4], clip.loop_start) && clip.loop_start >= 0 &&
                 static_cast<std::size_t>(clip.loop_start) < f.size() - 5;
            if (ok) {
                for (std::size_t i = 5; i < f.size(); ++i)
                    clip.frames.emplace_back(f[i]);
                std::string key{f[1]};
                for (auto& ch : key)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                clips[key] = std::move(clip);
            }
            break;
        }
        case 'P':
            ok = ParseFxRecord(f, fx);
            break;
        case 'O': {
            // O <sprite name> <left> <top> (the name may contain spaces: the numbers are last)
            const std::size_t n = f.size();
            int left{}, top{};
            ok = n >= 4 && Num(f[n - 2], left) && Num(f[n - 1], top);
            if (ok) {
                std::string name{f[1]};
                for (std::size_t i = 2; i + 2 < n; ++i)
                    name += " " + std::string{f[i]};
                offsets[std::move(name)] = {left, top};
            }
            break;
        }
        default:
            ok = false;
        }
        if (!ok)
            return false;
    }
    return version && !nodes.empty();
}

std::size_t ClipFrame(const PictureIndex::Clip& clip, double ms) {
    const auto n = static_cast<long long>(clip.frames.size());
    if (n <= 1 || ms < 0)
        return 0;
    const auto i = static_cast<long long>(ms * clip.fps / 1000.0);
    switch (clip.wrap) {
    case 0: // Loop
        return static_cast<std::size_t>(i % n);
    case 1: { // LoopSection: once up to loop_start, then loop the rest
        const long long ls = std::clamp<long long>(clip.loop_start, 0, n - 1);
        return static_cast<std::size_t>(i < n ? i : ls + (i - ls) % (n - ls));
    }
    case 2: // Once
        return static_cast<std::size_t>(std::min(i, n - 1));
    case 3: { // PingPong
        const long long p = i % (2 * n - 2);
        return static_cast<std::size_t>(p < n ? p : 2 * n - 2 - p);
    }
    default: // RandomFrame / RandomLoop / Single: the first frame (no invented sequence)
        return 0;
    }
}

std::optional<Picture> DecodeRgba32(const std::uint8_t* data, std::size_t size, int width, int height) {
    if (!data || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        size < static_cast<std::size_t>(width) * height * 4)
        return std::nullopt;
    Picture p;
    p.width = static_cast<std::uint32_t>(width);
    p.height = static_cast<std::uint32_t>(height);
    p.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) // Unity stores rows bottom-up
        std::copy_n(data + static_cast<std::size_t>(height - 1 - y) * row, row, p.rgba.data() + y * row);
    return p;
}

Picture CropSprite(const Picture& t, const PictureIndex::Sprite& s, int scale) {
    Picture out;
    scale = std::clamp(scale, 1, 8);
    if (s.w <= 0 || s.h <= 0 || s.x < 0 || s.y < 0 || static_cast<std::uint32_t>(s.x + s.w) > t.width ||
        static_cast<std::uint32_t>(s.y + s.h) > t.height)
        return out;
    // a turned sprite (tk2d "flipped") is stored rotated: output (x, y) = stored
    // (w - 1 - y, h - 1 - x), i.e. the transpose turned 180 degrees (checked live 2026-11-03:
    // the plain transpose showed ui/ElementWind and the font upside down and mirrored)
    const int w = s.turned ? s.h : s.w, h = s.turned ? s.w : s.h;
    out.width = static_cast<std::uint32_t>(w * scale);
    out.height = static_cast<std::uint32_t>(h * scale);
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    for (int y = 0; y < h * scale; ++y) {
        for (int x = 0; x < w * scale; ++x) {
            const int ox = x / scale, oy = y / scale;
            const int sx = s.x + (s.turned ? s.w - 1 - oy : ox), sy = s.y + (s.turned ? s.h - 1 - ox : oy);
            const auto* src = &t.rgba[(static_cast<std::size_t>(sy) * t.width + sx) * 4];
            std::copy_n(src, 4, &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4]);
        }
    }
    return out;
}

struct Art::Impl {
    Romfs romfs;
    std::mutex mutex;
    bool tried{}, ok{};
    PictureIndex index;
    std::shared_ptr<ce_unity::Bundle> bundle;
    std::list<std::pair<int, std::shared_ptr<Picture>>> cache; // most recent first
    std::map<std::string, std::string, std::less<>> strings;
    std::set<std::string, std::less<>> missed; ///< sprite names reported missing (once each)
    std::vector<std::string> missed_new;
    // host font atlas
    bool font_built{};
    Picture font_atlas;
    std::uint32_t font_first{}, font_line{};
    std::vector<EdenDsmodFontGlyph> font_glyphs;
};

Art::Art(Romfs romfs) : impl{std::make_unique<Impl>()} {
    impl->romfs = std::move(romfs);
}
Art::~Art() = default;

bool Art::LoadLocked() {
    if (impl->tried)
        return impl->ok;
    // the romfs may not be ready on the first calls: only a parsed index ends the retries
    const std::size_t n = impl->romfs(std::string{IndexPath}.c_str(), 0, nullptr, 0);
    if (n == 0 || n > 16 * 1024 * 1024)
        return false;
    std::string text(n, '\0');
    if (impl->romfs(std::string{IndexPath}.c_str(), 0, text.data(), n) != n)
        return false;
    impl->tried = true;
    if (!impl->index.Parse(text))
        return false;
    const std::size_t size = impl->romfs(std::string{DataPath}.c_str(), 0, nullptr, 0);
    if (size == 0) {
        impl->tried = false; // game romfs not mounted yet
        impl->index = {};
        return false;
    }
    ce_unity::ByteSource src;
    src.size = size;
    src.read = [romfs = impl->romfs](std::uint64_t off, std::uint8_t* out, std::size_t len) {
        return romfs(std::string{DataPath}.c_str(), off, out, len) == len;
    };
    impl->bundle = ce_unity::Bundle::Open(std::move(src));
    impl->ok = impl->bundle != nullptr;
    return impl->ok;
}

std::string Art::String(std::string_view name) {
    std::scoped_lock lock{impl->mutex};
    return StringLocked(name);
}

void Art::MissLocked(std::string_view sprite) {
    if (impl->missed.size() < 512 && impl->missed.emplace(sprite).second)
        impl->missed_new.emplace_back(sprite);
}

std::vector<std::string> Art::TakeMissing() {
    std::unique_lock lock{impl->mutex, std::try_to_lock};
    if (!lock.owns_lock())
        return {};
    return std::exchange(impl->missed_new, {});
}

std::map<std::string, PictureIndex::Clip, std::less<>> Art::Clips() {
    std::scoped_lock lock{impl->mutex};
    if (!LoadLocked())
        return {};
    return impl->index.clips;
}

std::set<std::string, std::less<>> Art::FxSets() {
    std::scoped_lock lock{impl->mutex};
    std::set<std::string, std::less<>> out;
    if (LoadLocked())
        for (const auto& [name, set] : impl->index.fx)
            out.insert(name);
    return out;
}

std::optional<Picture> Art::FxPicture(std::string_view set, double seconds, int frame, std::uint32_t width,
                                      std::uint32_t height, double ax, double ay, int scale) {
    std::scoped_lock lock{impl->mutex};
    if (!LoadLocked() || width == 0 || height == 0 || width > 4096 || height > 4096)
        return std::nullopt;
    const auto it = impl->index.fx.find(set);
    if (it == impl->index.fx.end())
        return std::nullopt;
    Picture p;
    p.width = width;
    p.height = height;
    p.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    DrawFxLocked(it->first, it->second, seconds, frame, p, ax, ay, scale);
    return p;
}

void Art::DrawFxLocked(std::string_view name, const FxSet& set, double seconds, int frame, Picture& out,
                       double ax, double ay, int scale) {
    std::map<std::string, Picture, std::less<>> textures;
    DrawFx(set, name, seconds, frame, out, ax, ay, scale, [&](std::string_view tex) -> const Picture* {
        if (const auto t = textures.find(tex); t != textures.end())
            return &t->second;
        const auto s = impl->index.sprites.find(tex);
        if (s == impl->index.sprites.end())
            return nullptr;
        const auto pixels = TextureLocked(s->second.tex);
        if (!pixels)
            return nullptr;
        return &textures.emplace(std::string{tex}, CropSprite(*pixels, s->second, 1)).first->second;
    });
}

const PictureIndex::Clip* Art::ClipLocked(std::string_view sprite) {
    if (!LoadLocked())
        return nullptr;
    std::string set{sprite.substr(0, sprite.find('/'))};
    for (auto& ch : set)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto it = impl->index.clips.find(set);
    return it == impl->index.clips.end() || it->second.frames.empty() ? nullptr : &it->second;
}

std::string Art::StringLocked(std::string_view name) {
    if (!LoadLocked())
        return {};
    if (const auto it = impl->strings.find(name); it != impl->strings.end())
        return it->second;
    const auto it = impl->index.strings.find(name);
    if (it == impl->index.strings.end())
        return {};
    const auto& t = it->second;
    if (t.node < 0 || static_cast<std::size_t>(t.node) >= impl->index.nodes.size())
        return {};
    const auto* node = impl->bundle->FindNode(impl->index.nodes[t.node]);
    if (!node || t.offset > node->size || t.bytes + 4 > node->size - t.offset)
        return {};
    std::vector<std::uint8_t> raw(t.bytes + 4);
    if (!impl->bundle->Read(node->offset + t.offset, raw.data(), raw.size()))
        return {};
    const std::uint32_t size = raw[0] | std::uint32_t{raw[1]} << 8 |
                               std::uint32_t{raw[2]} << 16 | std::uint32_t{raw[3]} << 24;
    if (size != t.bytes)
        return {};
    std::string text(raw.begin() + 4, raw.end());
    if (text.find('\0') != std::string::npos)
        return {};
    impl->strings[std::string{name}] = text;
    return text;
}

std::shared_ptr<const Picture> Art::TextureLocked(int id) {
    for (auto it = impl->cache.begin(); it != impl->cache.end(); ++it) {
        if (it->first == id) {
            impl->cache.splice(impl->cache.begin(), impl->cache, it);
            return impl->cache.front().second;
        }
    }
    const auto t = impl->index.textures.find(id);
    if (t == impl->index.textures.end() || (t->second.format != 4 /* RGBA32 */ && t->second.format != 3 /* RGB24 */) ||
        t->second.node < 0 || static_cast<std::size_t>(t->second.node) >= impl->index.nodes.size())
        return nullptr;
    const auto* node = impl->bundle->FindNode(impl->index.nodes[static_cast<std::size_t>(t->second.node)]);
    // subtraction, not offset + bytes > size: offset/bytes are read straight from pictures.idx
    // with no upper bound at parse time, and offset + bytes can wrap past node->size on a
    // corrupt/mismatched index, which would otherwise pass the check it was meant to fail
    if (!node || t->second.bytes > 64u * 1024 * 1024 || t->second.offset > node->size ||
        t->second.bytes > node->size - t->second.offset)
        return nullptr;
    std::vector<std::uint8_t> bytes(t->second.bytes);
    if (!impl->bundle->Read(node->offset + t->second.offset, bytes.data(), bytes.size()))
        return nullptr;
    if (t->second.format == 3) {
        const auto pixels = static_cast<std::uint64_t>(t->second.width) * t->second.height;
        if (t->second.width <= 0 || t->second.height <= 0 || pixels > bytes.size() / 3)
            return nullptr;
        std::vector<std::uint8_t> rgba(pixels * 4, 255);
        for (std::size_t i = 0; i < pixels; ++i)
            std::copy_n(bytes.data() + i * 3, 3, rgba.data() + i * 4);
        bytes = std::move(rgba);
    }
    auto pic = DecodeRgba32(bytes.data(), bytes.size(), t->second.width, t->second.height);
    if (!pic)
        return nullptr;
    impl->cache.emplace_front(id, std::make_shared<Picture>(std::move(*pic)));
    while (impl->cache.size() > MaxCachedTextures)
        impl->cache.pop_back();
    return impl->cache.front().second;
}

bool Art::BuildFontLocked() {
    if (impl->font_built)
        return !impl->font_glyphs.empty();
    impl->font_built = true;
    const auto f = impl->index.fonts.find(HostFontName);
    if (f == impl->index.fonts.end() || f->second.glyphs.empty())
        return false;
    const auto tex = TextureLocked(f->second.tex);
    if (!tex)
        return false;
    // baseline: the bottom of 'H'
    int baseline = 0;
    if (const auto h = f->second.glyphs.find('H'); h != f->second.glyphs.end())
        baseline = h->second.bottom;
    const std::uint32_t first = f->second.glyphs.begin()->first;
    const std::uint32_t last = std::min<std::uint32_t>(f->second.glyphs.rbegin()->first, first + 1023);
    // shelf-pack the unturned glyphs into a 256-wide atlas, 1px apart
    constexpr int AtlasW = 256;
    int x = 0, y = 0, shelf = 0;
    struct Placed {
        const PictureIndex::Glyph* g;
        int x, y;
    };
    std::map<std::uint32_t, Placed> placed;
    for (const auto& [cp, g] : f->second.glyphs) {
        if (cp > last)
            break;
        const int w = g.turned ? g.h : g.w, h = g.turned ? g.w : g.h;
        if (x + w + 1 > AtlasW) {
            x = 0;
            y += shelf + 1;
            shelf = 0;
        }
        placed[cp] = {&g, x, y};
        x += w + 1;
        shelf = std::max(shelf, h);
    }
    Picture atlas;
    atlas.width = AtlasW;
    atlas.height = static_cast<std::uint32_t>(std::max(1, y + shelf + 1));
    atlas.rgba.assign(static_cast<std::size_t>(atlas.width) * atlas.height * 4, 0);
    std::vector<EdenDsmodFontGlyph> glyphs(last - first + 1);
    for (const auto& [cp, p] : placed) {
        const auto& g = *p.g;
        const PictureIndex::Sprite s{f->second.tex, g.x, g.y, g.w, g.h, g.turned};
        const Picture crop = CropSprite(*tex, s, 1);
        for (std::uint32_t yy = 0; yy < crop.height; ++yy)
            for (std::uint32_t xx = 0; xx < crop.width; ++xx) {
                const auto* src = &crop.rgba[(yy * crop.width + xx) * 4];
                auto* dst = &atlas.rgba[((p.y + yy) * atlas.width + p.x + xx) * 4];
                // as stored: white letter inside the font's own 1 px dark outline; the host's
                // colour multiplies, so the outline stays dark
                std::copy_n(src, 4, dst);
            }
        auto& h = glyphs[cp - first];
        h.x = static_cast<std::uint16_t>(p.x);
        h.y = static_cast<std::uint16_t>(p.y);
        h.w = static_cast<std::uint16_t>(crop.width);
        h.h = static_cast<std::uint16_t>(crop.height);
        h.bearing_x = static_cast<std::int16_t>(g.bearing_x);
        h.bearing_y = static_cast<std::int16_t>(g.bearing_y - baseline);
        h.advance = static_cast<std::uint16_t>(std::max(0, g.advance));
    }
    impl->font_atlas = std::move(atlas);
    impl->font_first = first;
    impl->font_line = static_cast<std::uint32_t>(std::lround(f->second.line_height));
    impl->font_glyphs = std::move(glyphs);
    return true;
}

namespace {

/// One laid-out item of a text line: a code point, or an inline sprite ({i:<name>}).
struct TextItem {
    std::uint32_t cp{};
    std::string sprite; ///< non-empty: an inline icon
};

std::vector<TextItem> Items(std::string_view text) {
    std::vector<TextItem> out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i, 3) == "{i:") {
            const auto end = text.find('}', i);
            if (end != std::string_view::npos) {
                out.push_back({0, std::string{text.substr(i + 3, end - i - 3)}});
                i = end + 1;
                continue;
            }
        }
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) cp = c & 7, extra = 3;
        else if (c >= 0xE0) cp = c & 15, extra = 2;
        else if (c >= 0xC0) cp = c & 31, extra = 1;
        for (int k = 1; k <= extra && i + k < text.size(); ++k)
            cp = cp << 6 | (static_cast<unsigned char>(text[i + k]) & 63);
        i += 1 + extra;
        out.push_back({cp, {}});
    }
    return out;
}

constexpr int IconGap = 1; // px between the text and an inline icon, and between icons

} // namespace

std::optional<Picture> Art::TextLocked(std::string_view font, std::string_view text, int scale) {
    const auto f = impl->index.fonts.find(font);
    if (f == impl->index.fonts.end() || text.empty() || text.size() > 400)
        return std::nullopt;
    const auto tex = TextureLocked(f->second.tex);
    if (!tex)
        return std::nullopt;
    // {s:<name>}: the native string, substituted before layout
    std::string expanded;
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i, 3) == "{s:" || text.substr(i, 3) == "{S:") {
            const auto end = text.find('}', i);
            if (end != std::string_view::npos) {
                auto native = StringLocked(text.substr(i + 3, end - i - 3));
                if (text[i + 1] == 'S' && !native.empty()) // {S:..}: as a label, capital first letter
                    native[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(native[0])));
                expanded += native;
                i = end + 1;
                continue;
            }
        }
        expanded += text[i++];
    }
    int baseline = 0, cap_top = 0, cap_h = 7;
    const int line = static_cast<int>(std::lround(f->second.line_height));
    const int top = line - 2; // baseline sits 2 px above the line bottom (fits the 1 px descent)
    if (const auto h = f->second.glyphs.find('H'); h != f->second.glyphs.end()) {
        baseline = h->second.bottom;
        cap_h = h->second.turned ? h->second.w : h->second.h;
        cap_top = top - (h->second.bearing_y - baseline);
    }
    struct Placed {
        const PictureIndex::Sprite* sprite;
        int x;
    };
    const auto items = Items(expanded);
    std::vector<Placed> icons;
    int width = 0;
    for (const auto& it : items) {
        if (!it.sprite.empty()) {
            const auto s = impl->index.sprites.find(it.sprite);
            if (s == impl->index.sprites.end()) {
                MissLocked(it.sprite);
                continue;
            }
            width += IconGap;
            icons.push_back({&s->second, width});
            width += s->second.turned ? s->second.h : s->second.w;
            continue;
        }
        const auto g = f->second.glyphs.find(it.cp);
        width += g != f->second.glyphs.end() ? g->second.advance : 4;
    }
    width += 2;
    Picture out;
    out.width = static_cast<std::uint32_t>(std::max(1, width) * scale);
    out.height = static_cast<std::uint32_t>(line * scale);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    const auto blit = [&](const Picture& src, int gx, int gy) {
        for (std::uint32_t yy = 0; yy < src.height; ++yy)
            for (std::uint32_t xx = 0; xx < src.width; ++xx) {
                const auto* px = &src.rgba[(yy * src.width + xx) * 4];
                if (px[3] == 0)
                    continue;
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        const int ox = (gx + static_cast<int>(xx)) * scale + dx;
                        const int oy = (gy + static_cast<int>(yy)) * scale + dy;
                        if (ox < 0 || oy < 0 || ox >= static_cast<int>(out.width) || oy >= static_cast<int>(out.height))
                            continue;
                        std::copy_n(px, 4, &out.rgba[(static_cast<std::size_t>(oy) * out.width + ox) * 4]);
                    }
            }
    };
    int pen = 0;
    std::size_t icon = 0;
    for (const auto& it : items) {
        if (!it.sprite.empty()) {
            if (icon < icons.size() && impl->index.sprites.count(it.sprite)) {
                const auto& sp = *icons[icon].sprite;
                const auto itex = TextureLocked(sp.tex);
                // TextureLocked may evict the font texture from its cache: `tex` stays valid
                // (the shared_ptr keeps the decode alive even once it drops off the LRU list),
                // and the icon is centred on the capital height
                if (itex) {
                    const Picture ic = CropSprite(*itex, sp, 1);
                    const int y = std::clamp(cap_top + cap_h / 2 - static_cast<int>(ic.height) / 2, 0,
                                             std::max(0, line - static_cast<int>(ic.height)));
                    blit(ic, icons[icon].x, y);
                    pen = icons[icon].x + static_cast<int>(ic.width);
                }
                ++icon;
            }
            continue;
        }
        const auto g = f->second.glyphs.find(it.cp);
        if (g == f->second.glyphs.end()) {
            pen += 4;
            continue;
        }
        const auto& gl = g->second;
        const PictureIndex::Sprite sp{f->second.tex, gl.x, gl.y, gl.w, gl.h, gl.turned};
        blit(CropSprite(*tex, sp, 1), pen + gl.bearing_x, top - (gl.bearing_y - baseline));
        pen += gl.advance;
    }
    return out;
}

std::optional<std::vector<std::string>> Art::WrapLines(std::string_view font, int w, std::string_view text) {
    std::unique_lock lock{impl->mutex, std::try_to_lock};
    if (!lock.owns_lock() || !LoadLocked())
        return std::nullopt;
    const auto width_of = [&](std::string_view t) {
        const auto pic = TextLocked(font, t, 1);
        return pic ? static_cast<int>(pic->width) - 2 : 0;
    };
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < text.size();) {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i >= text.size())
            break;
        const auto sp = text.find(' ', i);
        const auto word = text.substr(i, sp == std::string_view::npos ? text.size() - i : sp - i);
        i = sp == std::string_view::npos ? text.size() : sp;
        const std::string next = cur.empty() ? std::string{word} : cur + " " + std::string{word};
        if (!cur.empty() && width_of(next) > w) {
            lines.push_back(std::move(cur));
            cur = std::string{word};
        } else {
            cur = next;
        }
    }
    if (!cur.empty())
        lines.push_back(std::move(cur));
    return lines;
}

std::optional<Picture> Art::WrapLocked(std::string_view key) {
    // <font>:<k>:<w>:<lines>:<text>  the text word-wrapped inside a box w mockup px wide and
    // <lines> lines tall (fixed size). "|" ends a line (one native point per line); a point's
    // continuation lines hang under its text after "- ". Fit rule: the named font, then regular,
    // then small; only when the small font still needs more lines is the last line cut "...".
    std::vector<std::string_view> parts;
    auto rest = key;
    for (int i = 0; i < 4; ++i) {
        const auto c = rest.find(':');
        if (c == std::string_view::npos)
            return std::nullopt;
        parts.push_back(rest.substr(0, c));
        rest.remove_prefix(c + 1);
    }
    int w{}, max_lines{};
    // "<n>c": every line centred, and the block centred in the box (tab captions)
    const bool centred = parts[3].ends_with('c');
    if (centred)
        parts[3].remove_suffix(1);
    if (parts[1].size() != 1 || parts[1][0] < '1' || parts[1][0] > '8' || !Num(parts[2], w) || w <= 0 ||
        w > 400 || !Num(parts[3], max_lines) || max_lines < 1 || max_lines > 12 || rest.empty())
        return std::nullopt;
    const int k = parts[1][0] - '0';
    std::vector<std::string_view> chain{parts[0]};
    for (const std::string_view f : {std::string_view{"regular"}, std::string_view{"small"}})
        if (std::find(chain.begin(), chain.end(), f) == chain.end())
            chain.push_back(f);
    struct Line {
        std::string text;
        int indent{};
    };
    const auto width_of = [&](std::string_view font, std::string_view t) {
        const auto pic = TextLocked(font, t, 1);
        return pic ? static_cast<int>(pic->width) - 2 : 0;
    };
    const auto wrap = [&](std::string_view font) {
        std::vector<Line> lines;
        std::string_view all = rest;
        while (true) {
            const auto bar = all.find('|');
            std::string_view para = all.substr(0, bar);
            while (!para.empty() && para.back() == ' ')
                para.remove_suffix(1);
            while (!para.empty() && para.front() == ' ')
                para.remove_prefix(1);
            if (!para.empty()) {
                const bool bullet = para.starts_with("- ");
                const int indent = bullet ? width_of(font, "- ") : 0;
                // words; the bullet mark stays glued to its first word
                std::vector<std::string_view> words;
                for (std::size_t i = bullet ? 2 : 0; i < para.size();) {
                    while (i < para.size() && para[i] == ' ')
                        ++i;
                    const auto sp = para.find(' ', i);
                    if (i < para.size())
                        words.push_back(para.substr(i, sp == std::string_view::npos ? para.size() - i : sp - i));
                    i = sp == std::string_view::npos ? para.size() : sp;
                }
                std::string cur = bullet ? "-" : "";
                int cur_indent = 0;
                for (const auto word : words) {
                    const std::string next = cur.empty() ? std::string{word} : cur + " " + std::string{word};
                    if (!cur.empty() && cur != "-" && cur_indent + width_of(font, next) > w) {
                        lines.push_back({cur, cur_indent});
                        cur = std::string{word};
                        cur_indent = indent;
                    } else {
                        cur = next;
                    }
                }
                if (!cur.empty())
                    lines.push_back({cur, cur_indent});
            }
            if (bar == std::string_view::npos)
                break;
            all.remove_prefix(bar + 1);
        }
        return lines;
    };
    std::vector<Line> lines;
    std::string_view used;
    for (const auto f : chain) {
        used = f;
        lines = wrap(f);
        if (static_cast<int>(lines.size()) <= max_lines &&
            std::all_of(lines.begin(), lines.end(), [&](const Line& l) { return l.indent + width_of(f, l.text) <= w; }))
            break;
    }
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.resize(static_cast<std::size_t>(max_lines));
        auto& last = lines.back();
        while (!last.text.empty() && last.indent + width_of(used, last.text + "...") > w)
            last.text.pop_back();
        last.text += "...";
    }
    const auto fo = impl->index.fonts.find(used);
    if (fo == impl->index.fonts.end())
        return std::nullopt;
    const int lh = static_cast<int>(std::lround(fo->second.line_height));
    // the box keeps the named font's height whichever font fitted (a fixed widget rect)
    const auto named = impl->index.fonts.find(parts[0]);
    const int box_lh = named != impl->index.fonts.end() ? static_cast<int>(std::lround(named->second.line_height)) : lh;
    const int box_h = max_lines * std::max(box_lh, lh);
    const int top = centred ? (box_h - static_cast<int>(lines.size()) * lh) / 2 : 0;
    Picture box;
    box.width = static_cast<std::uint32_t>(w * k);
    box.height = static_cast<std::uint32_t>(box_h * k);
    box.rgba.assign(static_cast<std::size_t>(box.width) * box.height * 4, 0);
    for (std::size_t n = 0; n < lines.size(); ++n) {
        const auto pic = TextLocked(used, lines[n].text, 1);
        if (!pic)
            continue;
        const int ox = centred ? std::max(0, (w - static_cast<int>(pic->width) + 2) / 2) : lines[n].indent;
        const int oy = top + static_cast<int>(n) * lh;
        for (std::uint32_t y = 0; y < pic->height; ++y)
            for (std::uint32_t x = 0; x < pic->width; ++x) {
                const int bx = ox + static_cast<int>(x), by = oy + static_cast<int>(y);
                if (bx < 0 || by < 0 || bx >= w || by >= box_h)
                    continue;
                const auto* px = &pic->rgba[(y * pic->width + x) * 4];
                for (int dy = 0; dy < k; ++dy)
                    for (int dx = 0; dx < k; ++dx)
                        std::copy_n(px, 4, &box.rgba[(static_cast<std::size_t>(by * k + dy) * box.width + bx * k + dx) * 4]);
            }
    }
    return box;
}

bool Art::Font(std::uint32_t& first, std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line) {
    std::scoped_lock lock{impl->mutex};
    if (!LoadLocked() || !BuildFontLocked())
        return false;
    first = impl->font_first;
    glyphs = impl->font_glyphs;
    line = impl->font_line;
    return true;
}

std::optional<Picture> Art::Load(std::string_view key) {
    if (!key.starts_with("module:ms:"))
        return std::nullopt;
    key.remove_prefix(10);
    std::scoped_lock lock{impl->mutex};
    if (!LoadLocked())
        return std::nullopt;
    if (key == "font") {
        if (!BuildFontLocked())
            return std::nullopt;
        return impl->font_atlas;
    }
    if (key.starts_with("txt:")) {
        // module:ms:txt:<font>:<k>:<w>:<l|c|r>:<text>  text in a game font at k times inside a box
        // w mockup px wide (the picture is exactly w*k wide, so a fixed widget rect never
        // stretches it). Fit rule of the mockups: the named font, then regular, then small,
        // then small cut with "...". White letters with the font's own dark outline.
        std::vector<std::string_view> parts;
        auto rest = key.substr(4);
        for (int i = 0; i < 4; ++i) {
            const auto c = rest.find(':');
            if (c == std::string_view::npos)
                return std::nullopt;
            parts.push_back(rest.substr(0, c));
            rest.remove_prefix(c + 1);
        }
        int k{}, w{};
        if (parts[1].size() != 1 || parts[1][0] < '1' || parts[1][0] > '8' || !Num(parts[2], w) || w <= 0 ||
            w > 400 || parts[3].size() != 1)
            return std::nullopt;
        k = parts[1][0] - '0';
        std::vector<std::string_view> chain{parts[0]};
        for (const std::string_view f : {std::string_view{"regular"}, std::string_view{"small"}})
            if (std::find(chain.begin(), chain.end(), f) == chain.end())
                chain.push_back(f);
        std::optional<Picture> line;
        for (const auto f : chain) {
            line = TextLocked(f, rest, 1);
            if (line && static_cast<int>(line->width) <= w)
                break;
        }
        if (!line)
            return std::nullopt;
        if (static_cast<int>(line->width) > w) {
            // cut the text, never the inline icons that follow it
            std::string cut{rest}, icons;
            while (cut.ends_with('}') && cut.rfind("{i:") != std::string::npos) {
                const auto at = cut.rfind("{i:");
                icons.insert(0, cut.substr(at));
                cut.resize(at);
            }
            while (!cut.empty()) {
                cut.pop_back();
                line = TextLocked("small", cut + "..." + icons, 1);
                if (line && static_cast<int>(line->width) <= w)
                    break;
            }
            if (!line)
                return std::nullopt;
        }
        Picture box;
        box.width = static_cast<std::uint32_t>(w * k);
        box.height = line->height * static_cast<std::uint32_t>(k);
        box.rgba.assign(static_cast<std::size_t>(box.width) * box.height * 4, 0);
        const int lw = static_cast<int>(line->width);
        // still wider than the box (inline icons alone overflow it, "..." cannot help): squeeze
        // the line horizontally into the box instead of clipping its last icons
        const bool squeeze = lw > w;
        const int ox = squeeze ? 0 : parts[3][0] == 'c' ? (w - lw) / 2 : parts[3][0] == 'r' ? w - lw : 0;
        for (std::uint32_t y = 0; y < box.height; ++y)
            for (std::uint32_t x = 0; x < box.width; ++x) {
                const int sx = squeeze ? static_cast<int>(static_cast<long>(x) * lw / (static_cast<long>(w) * k))
                                       : static_cast<int>(x) / k - ox;
                const int sy = static_cast<int>(y) / k;
                if (sx < 0 || sx >= lw || sy >= static_cast<int>(line->height))
                    continue;
                std::copy_n(&line->rgba[(static_cast<std::size_t>(sy) * line->width + sx) * 4], 4,
                            &box.rgba[(static_cast<std::size_t>(y) * box.width + x) * 4]);
            }
        return box;
    }
    if (key.starts_with("wrap:"))
        return WrapLocked(key.substr(5));
    if ((key.starts_with("box") || key.starts_with("row")) && key.size() > 5 && key[4] == ':' && key[3] >= '1' && key[3] <= '8') {
        // A monster bottom-centred in a 64x64 box, or clipped to a 64x54 Team row.
        // Lookup: the exact battle frame, the same name in any letter case (collections are not
        // consistently capitalised: "catzerker/0"), then the monster's still ("big/Vaero")
        const int scale = key[3] - '0';
        // "<frame sprite>~<set>": a frame of <set>'s idle clip (its sprite names may differ from
        // the set: Vaero plays "birdy/0".."birdy/9"); a bare sprite is its own set
        std::string want{key.substr(5)}, set_key;
        // "@<step>": the set's particle effects at step / FxSteps seconds into their loop
        int fx_step = -1;
        if (const auto at = want.rfind('@'); at != std::string::npos) {
            fx_step = std::atoi(want.c_str() + at + 1);
            want.resize(at);
        }
        if (const auto tilde = want.find('~'); tilde != std::string::npos) {
            set_key = want.substr(tilde + 1) + "/";
            want.resize(tilde);
        }
        auto s = impl->index.sprites.find(want);
        const auto lower = [](std::string v) {
            for (auto& ch : v)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        };
        if (s == impl->index.sprites.end()) {
            const std::string lw = lower(want);
            for (auto it = impl->index.sprites.begin(); it != impl->index.sprites.end(); ++it)
                if (lower(it->first) == lw) {
                    s = it;
                    break;
                }
        }
        if (s == impl->index.sprites.end()) {
            std::string base = want.substr(0, want.find('/'));
            for (const char* suffix : {"_light", "_dark"})
                if (base.ends_with(suffix))
                    base.resize(base.size() - std::strlen(suffix));
            s = impl->index.sprites.find("big/" + base);
        }
        if (s == impl->index.sprites.end()) {
            MissLocked(want);
            return std::nullopt;
        }
        const auto tex = TextureLocked(s->second.tex);
        if (!tex)
            return std::nullopt;
        // Idle clip frames are placed by their anchor offsets inside the union of the clip's
        // frames, so the frames line up as in the game; that union is what is bottom-centred.
        const auto dims = [](const PictureIndex::Sprite& sp) {
            return std::pair<int, int>{sp.turned ? sp.h : sp.w, sp.turned ? sp.w : sp.h};
        };
        auto [fw, fh] = dims(s->second);
        int fx = 0, fy = 0, uw = fw, uh = fh;
        if (const auto off = impl->index.offsets.find(s->first); off != impl->index.offsets.end()) {
            if (const auto* clip = ClipLocked(set_key.empty() ? s->first : set_key)) {
                int l = off->second.first, t = off->second.second, r = l + fw, b = t + fh;
                bool all = true;
                for (const auto& name : clip->frames) {
                    const auto fo = impl->index.offsets.find(name);
                    const auto fs = impl->index.sprites.find(name);
                    if (fo == impl->index.offsets.end() || fs == impl->index.sprites.end()) {
                        all = false;
                        break;
                    }
                    const auto [w2, h2] = dims(fs->second);
                    l = std::min(l, fo->second.first);
                    t = std::min(t, fo->second.second);
                    r = std::max(r, fo->second.first + w2);
                    b = std::max(b, fo->second.second + h2);
                }
                if (all) {
                    fx = off->second.first - l;
                    fy = off->second.second - t;
                    uw = r - l;
                    uh = b - t;
                }
            }
        }
        // a still (or clip) larger than the box (big/...) is shrunk by a whole factor first
        const int big = std::max(uw, uh);
        const int div = (big + 63) / 64;
        const Picture full = CropSprite(*tex, s->second, div > 1 ? 1 : scale);
        if (full.rgba.empty())
            return std::nullopt;
        Picture spr = full;
        if (div > 1) {
            spr.width = full.width * static_cast<std::uint32_t>(scale) / static_cast<std::uint32_t>(div);
            spr.height = full.height * static_cast<std::uint32_t>(scale) / static_cast<std::uint32_t>(div);
            spr.rgba.assign(static_cast<std::size_t>(spr.width) * spr.height * 4, 0);
            for (std::uint32_t y = 0; y < spr.height; ++y)
                for (std::uint32_t x = 0; x < spr.width; ++x)
                    std::copy_n(&full.rgba[((y * div / scale) * full.width + x * div / scale) * 4], 4,
                                &spr.rgba[(y * spr.width + x) * 4]);
        }
        Picture box;
        box.width = static_cast<std::uint32_t>(64 * scale);
        // Team rows retain the sprite scale and bottom anchor, clipping to their inner height.
        box.height = static_cast<std::uint32_t>((key.starts_with("row") ? 54 : 64) * scale);
        box.rgba.assign(static_cast<std::size_t>(box.width) * box.height * 4, 0);
        const int ox = (static_cast<int>(box.width) - uw * scale / div) / 2 + fx * scale / div;
        const int oy = static_cast<int>(box.height) - uh * scale / div + fy * scale / div;
        // the particle layer of the set (drawn over the sprite, like the game: same anchor, same
        // pixel grid); only for idle-clip frames, whose anchor offset is known
        const FxSet* fx_set = nullptr;
        std::string fx_name;
        int fx_frame = -1;
        double anchor_x = 0, anchor_y = 0;
        if (fx_step >= 0 && div == 1 && !set_key.empty()) {
            fx_name = set_key.substr(0, set_key.size() - 1);
            for (auto& ch : fx_name)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const auto it = impl->index.fx.find(fx_name);
            const auto off = impl->index.offsets.find(s->first);
            if (it != impl->index.fx.end() && off != impl->index.offsets.end()) {
                fx_set = &it->second;
                fx_frame = FrameNumber(s->first);
                anchor_x = ox - off->second.first * scale;
                anchor_y = oy - off->second.second * scale;
            }
        }
        for (std::uint32_t y = 0; y < spr.height; ++y)
            for (std::uint32_t x = 0; x < spr.width; ++x) {
                const int dx = ox + static_cast<int>(x), dy = oy + static_cast<int>(y);
                if (dx < 0 || dy < 0 || dx >= static_cast<int>(box.width) || dy >= static_cast<int>(box.height))
                    continue;
                std::copy_n(&spr.rgba[(y * spr.width + x) * 4], 4,
                            &box.rgba[(static_cast<std::size_t>(dy) * box.width + dx) * 4]);
            }
        if (fx_set) {
            const auto* clip = ClipLocked(set_key);
            const double fps = clip && clip->fps > 0 ? clip->fps : 15.0;
            DrawFxLocked(fx_name, *fx_set, fx_step / fps, fx_frame, box, anchor_x, anchor_y, scale);
        }
        return box;
    }
    if (key.starts_with("spr") && key.size() > 5 && key[4] == ':' && key[3] >= '1' && key[3] <= '8') {
        const int scale = key[3] - '0';
        const auto s = impl->index.sprites.find(key.substr(5));
        if (s == impl->index.sprites.end()) {
            MissLocked(key.substr(5));
            return std::nullopt;
        }
        const auto tex = TextureLocked(s->second.tex);
        if (!tex)
            return std::nullopt;
        Picture p = CropSprite(*tex, s->second, scale);
        if (p.rgba.empty())
            return std::nullopt;
        return p;
    }
    return std::nullopt;
}

} // namespace ms
