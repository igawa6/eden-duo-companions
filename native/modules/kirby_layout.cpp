// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#include "kirby_layout.h"

#include <cstring>
#include <string_view>

namespace KirbyLayout {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;

struct Reader {
    std::span<const u8> d;
    bool Fits(std::size_t o, std::size_t n) const { return o <= d.size() && n <= d.size() - o; }
    u32 U16(std::size_t o) const { return Fits(o, 2) ? d[o] | d[o + 1] << 8 : 0; }
    u32 U32(std::size_t o) const { return Fits(o, 4) ? U16(o) | U16(o + 2) << 16 : 0; }
    float F32(std::size_t o) const {
        const u32 v = U32(o);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string Str(std::size_t o, std::size_t max) const {
        std::string s;
        for (; Fits(o, 1) && max > 0 && d[o] != 0; ++o, --max)
            s += static_cast<char>(d[o]);
        return s;
    }
};

std::vector<std::string> Names(const Reader& r, std::size_t at) {
    std::vector<std::string> out;
    const u32 n = r.U16(at + 8);
    for (u32 k = 0; k < n && r.Fits(at + 12 + k * 4, 4); ++k)
        out.push_back(r.Str(at + 12 + r.U32(at + 12 + k * 4), 128));
    return out;
}
} // namespace

std::optional<Layout> Parse(std::span<const u8> bytes) {
    const Reader r{bytes};
    if (!r.Fits(0, 0x14) || std::memcmp(bytes.data(), "FLYT", 4) != 0)
        return std::nullopt;
    Layout out;
    std::vector<int> stack;
    int last = -1;
    std::size_t at = r.U16(6);
    for (u32 left = r.U16(0x10); left > 0 && r.Fits(at, 8); --left) {
        const std::string_view magic{reinterpret_cast<const char*>(bytes.data() + at), 4};
        const u32 size = r.U32(at + 4);
        if (size < 8 || !r.Fits(at, size))
            return std::nullopt;
        if (magic == "txl1") {
            out.textures = Names(r, at);
        } else if (magic == "mat1") {
            for (u32 k = 0, n = r.U16(at + 8); k < n; ++k) {
                const std::size_t m = at + r.U32(at + 12 + k * 4);
                Material mat;
                mat.name = r.Str(m, 28);
                if (r.Fits(m, 48)) {
                    std::memcpy(mat.black, bytes.data() + m + 36, 4);
                    std::memcpy(mat.white, bytes.data() + m + 40, 4);
                    for (u32 t = 0, n = r.U32(m + 28) & 3; t < n; ++t)
                        mat.textures.push_back(static_cast<int>(r.U16(m + 44 + t * 4)));
                }
                out.materials.push_back(std::move(mat));
            }
        } else if (magic == "pan1" || magic == "pic1" || magic == "txt1" || magic == "wnd1" ||
                   magic == "prt1" || magic == "bnd1") {
            Pane p;
            p.kind = std::string{magic};
            p.parent = stack.empty() ? -1 : stack.back();
            p.visible = (r.U16(at + 8) & 1) != 0;
            p.alpha = static_cast<u8>(r.U16(at + 0xA) & 0xFF);
            p.name = r.Str(at + 0xC, 24);
            p.tx = r.F32(at + 0x2C);
            p.ty = r.F32(at + 0x30);
            p.sx = r.F32(at + 0x44);
            p.sy = r.F32(at + 0x48);
            p.w = r.F32(at + 0x4C);
            p.h = r.F32(at + 0x50);
            if (magic == "pic1")
                p.material = static_cast<int>(r.U16(at + 0x54 + 16));
            out.panes.push_back(std::move(p));
            last = static_cast<int>(out.panes.size()) - 1;
        } else if (magic == "pas1") {
            stack.push_back(last);
        } else if (magic == "pae1") {
            if (!stack.empty())
                stack.pop_back();
        }
        at += size;
    }
    return out;
}

} // namespace KirbyLayout
