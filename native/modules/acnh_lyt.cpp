// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_lyt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>

#include "acnh_bytes.h"
#include "acnh_draw.h"
#include "acnh_romfs.h"
#include "acnh_tex.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh {
namespace {
struct R {
    const std::vector<uint8_t>& d;
    uint16_t U16(size_t o) const {
        return bytes::Le16(d, o);
    }
    uint32_t U32(size_t o) const {
        return bytes::Le32(d, o);
    }
    float F32(size_t o) const {
        return bytes::F32(d, o);
    }
    uint8_t U8(size_t o) const {
        return bytes::U8(d, o);
    }
    std::string Str(size_t o, size_t max) const {
        std::string s;
        for (size_t i = 0; i < max && o + i < d.size() && d[o + i]; ++i)
            s += static_cast<char>(d[o + i]);
        return s;
    }
    Rgba Col(size_t o) const {
        return {U8(o), U8(o + 1), U8(o + 2), U8(o + 3)};
    }
};

constexpr size_t MaxDepth = 64; ///< pane nesting (3.0.3: at most 15 levels over all 959 layouts)

} // namespace

bool Bflyt::Parse(const std::vector<uint8_t>& b) {
    if (b.size() < 0x14 || std::memcmp(b.data(), "FLYT", 4) != 0)
        return false;
    const R r{b};
    size_t o = r.U16(6);
    std::vector<int> stack{-1};
    int last = -1;
    while (o + 8 <= b.size()) {
        const std::string tag(reinterpret_cast<const char*>(b.data() + o), 4);
        const uint32_t size = r.U32(o + 4);
        if (size < 8 || o + size > b.size())
            break;
        const size_t s = o;
        if (tag == "txl1" || tag == "fnl1") {
            const uint16_t n = r.U16(s + 8);
            std::vector<std::string> names;
            for (uint16_t i = 0; i < n; ++i)
                names.push_back(r.Str(s + 0xC + r.U32(s + 0xC + 4 * i), 256));
            if (tag == "txl1")
                textures = std::move(names);
        } else if (tag == "mat1") {
            const uint16_t n = r.U16(s + 8);
            for (uint16_t i = 0; i < n; ++i) {
                const size_t mo = s + r.U32(s + 0xC + 4 * i);
                LytMaterial m;
                m.name = r.Str(mo, 0x1C);
                const uint32_t flags = r.U32(mo + 0x1C);
                m.black = r.Col(mo + 0x24);
                m.white = r.Col(mo + 0x28);
                for (uint32_t t = 0; t < (flags & 3); ++t) {
                    const uint16_t ti = r.U16(mo + 0x2C + 4 * t);
                    m.tex.push_back(ti < textures.size() ? textures[ti] : std::string{});
                }
                materials.push_back(std::move(m));
            }
        } else if (tag == "pan1" || tag == "pic1" || tag == "txt1" || tag == "wnd1" ||
                   tag == "prt1" || tag == "bnd1" || tag == "scr1" || tag == "ali1" ||
                   tag == "cpt1") {
            LytPane p;
            p.tag = tag;
            p.flags = r.U8(s + 8);
            p.origin = r.U8(s + 9);
            p.alpha = r.U8(s + 10);
            p.name = r.Str(s + 0xC, 0x18);
            p.x = r.F32(s + 0x2C);
            p.y = r.F32(s + 0x30);
            p.rz = r.F32(s + 0x40);
            p.sx = r.F32(s + 0x44);
            p.sy = r.F32(s + 0x48);
            p.w = r.F32(s + 0x4C);
            p.h = r.F32(s + 0x50);
            p.visible = (p.flags & 1) != 0;
            for (auto& c : p.vtx)
                c = {255, 255, 255, 255};
            if (tag == "pic1") {
                for (int i = 0; i < 4; ++i)
                    p.vtx[i] = r.Col(s + 0x54 + 4 * i);
                p.material = r.U16(s + 0x64);
                if (p.material >= static_cast<int>(materials.size()))
                    p.material = -1;
            }
            const int idx = static_cast<int>(panes.size());
            if (stack.back() >= 0)
                panes[stack.back()].children.push_back(idx);
            panes.push_back(std::move(p));
            last = idx;
        } else if (tag == "pas1") {
            if (stack.size() > MaxDepth)
                return false; // Compose recurses per level: no unbounded nesting
            stack.push_back(last);
        } else if (tag == "pae1") {
            if (stack.size() > 1)
                stack.pop_back();
        }
        o += size;
    }
    return !panes.empty();
}

int Bflyt::Find(std::string_view pane) const {
    for (size_t i = 0; i < panes.size(); ++i)
        if (panes[i].name == pane)
            return static_cast<int>(i);
    return -1;
}

int Bflyt::Material(std::string_view name) const {
    for (size_t i = 0; i < materials.size(); ++i)
        if (materials[i].name == name)
            return static_cast<int>(i);
    return -1;
}

bool Bflan::Parse(const std::vector<uint8_t>& b) {
    if (b.size() < 0x14 || std::memcmp(b.data(), "FLAN", 4) != 0)
        return false;
    const R r{b};
    size_t o = r.U16(6);
    while (o + 8 <= b.size()) {
        const uint32_t size = r.U32(o + 4);
        if (size == 0 || o + size > b.size())
            break;
        if (std::memcmp(b.data() + o, "pai1", 4) == 0) {
            // every table must lie inside the section: the counts alone (u16 x u8 x u8 x u16)
            // would let a malformed file loop ~2^48 times
            const size_t end = o + size;
            const auto inside = [&](size_t at, size_t n) { return at <= end && n <= end - at; };
            const uint16_t ntex = r.U16(o + 12), nent = r.U16(o + 14);
            const uint32_t eoff = r.U32(o + 16);
            for (uint16_t k = 0; k < ntex && inside(o + 0x14 + 4 * k, 4); ++k)
                textures.push_back(r.Str(o + 0x14 + r.U32(o + 0x14 + 4 * k), 256));
            for (uint16_t e = 0; e < nent && inside(o + eoff + 4 * size_t{e}, 4); ++e) {
                const size_t eo = o + r.U32(o + eoff + 4 * e);
                if (!inside(eo, 0x20))
                    break;
                BflanEntry entry;
                entry.name = r.Str(eo, 0x1C);
                const uint8_t ntag = r.U8(eo + 0x1C);
                for (uint8_t t = 0; t < ntag && inside(eo + 0x20 + 4 * t, 4); ++t) {
                    const size_t to = eo + r.U32(eo + 0x20 + 4 * t);
                    if (!inside(to, 8))
                        break;
                    const std::string magic(reinterpret_cast<const char*>(b.data() + to), 4);
                    const uint8_t n = r.U8(to + 4);
                    for (uint8_t k = 0; k < n && inside(to + 8 + 4 * k, 4); ++k) {
                        const size_t te = to + r.U32(to + 8 + 4 * k);
                        if (!inside(te, 12))
                            break;
                        BflanTag tag;
                        tag.magic = magic;
                        tag.index = r.U8(te);
                        tag.target = r.U8(te + 1);
                        tag.curve = r.U8(te + 2);
                        const uint32_t ko = r.U32(te + 8);
                        const size_t stride = tag.curve == 1 ? 8 : 12;
                        uint16_t nk = r.U16(te + 4);
                        if (!inside(te + ko, stride * nk))
                            nk = 0; // keys outside the section: none
                        for (uint16_t q = 0; q < nk; ++q) {
                            if (tag.curve == 1)
                                tag.keys.push_back(
                                    {r.F32(te + ko + 8 * q),
                                     static_cast<float>(r.U16(te + ko + 8 * q + 4))});
                            else
                                tag.keys.push_back(
                                    {r.F32(te + ko + 12 * q),
                                     std::round(r.F32(te + ko + 12 * q + 4) * 1000.0f) / 1000.0f});
                        }
                        entry.tags.push_back(std::move(tag));
                    }
                }
                entries.push_back(std::move(entry));
            }
        }
        o += size;
    }
    return true;
}

std::optional<float> Bflan::At(std::string_view entry, std::string_view magic, int index,
                               int target, float frame) const {
    std::optional<float> out;
    for (const auto& e : entries) {
        if (e.name != entry)
            continue;
        for (const auto& t : e.tags) {
            if (t.magic != magic || t.index != index || t.target != target || t.keys.empty())
                continue;
            float v = t.keys[0].value;
            for (const auto& k : t.keys)
                if (k.frame <= frame)
                    v = k.value;
            out = v; // a later entry of the same name wins (dict semantics of the research tool)
        }
    }
    return out;
}

std::shared_ptr<Layout> Layout::Load(Romfs& romfs, std::string_view name) {
    const auto arc = romfs.Layout(name);
    if (!arc)
        return nullptr;
    auto l = std::make_shared<Layout>();
    for (const auto& [member, bytes] : arc->members) {
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "FLYT", 4) == 0 &&
            l->tree.panes.empty()) {
            l->tree.Parse(bytes);
        } else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "FLAN", 4) == 0) {
            const auto slash = member.rfind('/');
            std::string base = member.substr(slash == std::string::npos ? 0 : slash + 1);
            if (base.ends_with(".bflan"))
                base.resize(base.size() - 6);
            Bflan a;
            if (a.Parse(bytes))
                l->anims.emplace(std::move(base), std::move(a));
        } else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "BNTX", 4) == 0 && !l->bntx) {
            l->bntx = &bytes;
        }
    }
    l->archive = arc;
    return l->tree.panes.empty() && !l->bntx ? nullptr : l;
}

const Bflan* Layout::Anim(std::string_view name) const {
    const auto it = anims.find(name);
    return it == anims.end() ? nullptr : &it->second;
}

std::shared_ptr<const Image> Layout::Texture(std::string_view name) const {
    {
        std::scoped_lock lock{tex_mutex};
        if (const auto it = tex_cache.find(name); it != tex_cache.end())
            return it->second;
    }
    if (!bntx)
        return nullptr;
    // decoded outside the texture-cache lock (milliseconds per texture: the timing thread's map
    // layouts must not wait for the asset worker's decodes); the first one stored wins
    auto img = std::make_shared<Image>();
    if (!BntxDecode(*bntx, name, *img))
        return nullptr;
    std::scoped_lock lock{tex_mutex};
    return tex_cache.emplace(std::string{name}, std::move(img)).first->second;
}

void Layout::AnimMaterial(std::string_view anim, float frame, std::string_view material,
                          Rgba& black, Rgba& white) const {
    const Bflan* a = Anim(anim);
    if (!a)
        return;
    std::array<uint8_t, 8> c{black.r, black.g, black.b, black.a,
                             white.r, white.g, white.b, white.a};
    for (int t = 0; t < 8; ++t)
        if (const auto v = a->At(material, "FLMC", 0, t, frame); v && std::isfinite(*v))
            c[t] = static_cast<uint8_t>(std::nearbyint(std::clamp(*v, 0.0f, 255.0f)));
    black = {c[0], c[1], c[2], c[3]};
    white = {c[4], c[5], c[6], c[7]};
}

bool Layout::AnimVisible(std::string_view anim, float frame, std::string_view pane,
                         bool fallback) const {
    const Bflan* a = Anim(anim);
    if (!a)
        return fallback;
    const auto v = a->At(pane, "FLVI", 0, 0, frame);
    return v ? *v != 0 : fallback;
}

std::string Layout::AnimTexture(std::string_view anim, float frame,
                                std::string_view material) const {
    const Bflan* a = Anim(anim);
    if (!a)
        return {};
    const auto v = a->At(material, "FLTP", 0, 0, frame);
    if (!v || !(*v >= 0.0f && *v < static_cast<float>(a->textures.size()))) // also NaN: no UB cast
        return {};
    const size_t i = static_cast<size_t>(*v);
    return i < a->textures.size() ? a->textures[i] : std::string{};
}

Image Layout::Compose(std::string_view root_name, const ComposeOptions& opt) const {
    Image canvas{opt.canvas_w, opt.canvas_h};
    const int root = tree.Find(root_name);
    if (root < 0)
        return canvas;
    using M = std::array<double, 6>; // [a b c; d e f]
    const auto mul = [](const M& p, const M& q) {
        return M{p[0] * q[0] + p[1] * q[3],        p[0] * q[1] + p[1] * q[4],
                 p[0] * q[2] + p[1] * q[5] + p[2], p[3] * q[0] + p[4] * q[3],
                 p[3] * q[1] + p[4] * q[4],        p[3] * q[2] + p[4] * q[5] + p[5]};
    };
    const M base{opt.scale, 0,          opt.canvas_w / 2.0 - opt.center_x * opt.scale,
                 0,         -opt.scale, opt.canvas_h / 2.0 + opt.center_y * opt.scale};
    const auto draw_pane = [&](auto&& self, int pi, const M& parent, double alpha) -> void {
        const LytPane& p = tree.panes[pi];
        const bool vis =
            opt.anim.empty() ? p.visible : AnimVisible(opt.anim, opt.frame, p.name, p.visible);
        if (!vis || std::find(opt.hide.begin(), opt.hide.end(), p.name) != opt.hide.end())
            return;
        const double a = p.rz * std::numbers::pi / 180.0;
        const M t{1, 0, p.x, 0, 1, p.y};
        const M rot{std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0};
        const M s{p.sx, 0, 0, 0, p.sy, 0};
        const M mp = mul(mul(mul(parent, t), rot), s);
        const double al = (p.flags & 2) ? alpha * (p.alpha / 255.0) : alpha;
        if (p.tag == "pic1" && p.material >= 0) {
            LytMaterial mat = tree.materials[p.material];
            if (!opt.anim.empty()) {
                AnimMaterial(opt.anim, opt.frame, mat.name, mat.black, mat.white);
                if (auto tx = AnimTexture(opt.anim, opt.frame, mat.name); !tx.empty()) {
                    if (mat.tex.empty())
                        mat.tex.push_back(tx);
                    else
                        mat.tex[0] = tx;
                }
            }
            const auto tex = !mat.tex.empty() ? Texture(mat.tex[0]) : nullptr;
            if (tex && !tex->Empty()) {
                const bool plain = std::all_of(p.vtx.begin(), p.vtx.end(), [](const Rgba& c) {
                    return c.r == 255 && c.g == 255 && c.b == 255 && c.a == 255;
                });
                Image t;
                if (plain) {
                    t = draw::Tint(*tex, mat.black, mat.white);
                } else {
                    // mean of the 4 vertex colours, float32 like numpy
                    float m[4] = {0, 0, 0, 0};
                    for (const auto& c : p.vtx) {
                        m[0] += c.r;
                        m[1] += c.g;
                        m[2] += c.b;
                        m[3] += c.a;
                    }
                    const float avg[4] = {m[0] / 4 / 255.0f, m[1] / 4 / 255.0f, m[2] / 4 / 255.0f,
                                          m[3] / 4 / 255.0f};
                    t = draw::TintVtx(*tex, mat.black, mat.white, avg);
                }
                const double k = al * p.alpha / 255.0;
                if (k < 1.0)
                    for (size_t q = 3; q < t.rgba.size(); q += 4)
                        t.rgba[q] = static_cast<uint8_t>(t.rgba[q] * k);
                const int ox = p.origin % 4, oy = (p.origin / 4) % 4;
                const double x0 = ox == 0 ? -p.w / 2.0 : ox == 1 ? 0.0 : -p.w;
                const double y1 = oy == 0 ? p.h / 2.0 : oy == 1 ? 0.0 : p.h;
                const M q{p.w / t.w, 0, x0, 0, -p.h / t.h, y1};
                const M f = mul(mp, q);
                const double det = f[0] * f[4] - f[1] * f[3];
                if (std::abs(det) > 1e-12) {
                    const double inv[6] = {
                        f[4] / det,  -f[1] / det, (f[1] * f[5] - f[4] * f[2]) / det,
                        -f[3] / det, f[0] / det,  (f[3] * f[2] - f[0] * f[5]) / det};
                    draw::AffineComposite(canvas, t, inv);
                }
            }
        }
        for (const int c : p.children)
            self(self, c, mp, al);
    };
    for (const int c : tree.panes[root].children)
        draw_pane(draw_pane, c, base, 1.0);
    return canvas;
}

std::shared_ptr<Layout> LoadLayout(Romfs& romfs, std::string_view name) {
    return romfs.CachedLayout(name);
}

} // namespace acnh
