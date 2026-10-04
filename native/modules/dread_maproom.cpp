// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dread_maproom.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

#include "dread_mapgen_util.h"

namespace dread_mapgen {
namespace {

// Bounds-checked little-endian cursor over the file.
struct Cursor {
    std::span<const std::uint8_t> d;
    std::size_t at = 0;
    bool bad = false;

    bool Need(std::size_t n) {
        if (bad || at > d.size() || d.size() - at < n) {
            bad = true;
            return false;
        }
        return true;
    }
    template <class T>
    T Get() {
        T v{};
        if (Need(sizeof(T))) {
            std::memcpy(&v, d.data() + at, sizeof(T));
            at += sizeof(T);
        }
        return v;
    }
    float F32() {
        return Get<float>();
    }
    std::string_view Str() {
        if (bad)
            return {};
        const auto* begin = d.data() + at;
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(begin, 0, d.size() - at));
        if (!end) {
            bad = true;
            return {};
        }
        at += std::size_t(end - begin) + 1;
        return {reinterpret_cast<const char*>(begin), std::size_t(end - begin)};
    }
    void Skip(std::size_t n) {
        if (Need(n))
            at += n;
    }
    Cursor At(std::uint64_t offset) const {
        Cursor c{d, 0, bad};
        if (offset > d.size())
            c.bad = true;
        else
            c.at = std::size_t(offset);
        return c;
    }
};

bool Fail(std::string* err, const char* what) {
    if (err)
        *err = what;
    return false;
}

// A Mercury linked-list TOC: (ptr, next) pairs, contiguous, the last with next == 0.
std::vector<std::uint64_t> SubToc(Cursor c) {
    std::vector<std::uint64_t> ptrs;
    for (int guard = 0; guard < 1 << 16; ++guard) {
        const auto ptr = c.Get<std::uint64_t>();
        const auto next = c.Get<std::uint64_t>();
        if (c.bad)
            return {};
        ptrs.push_back(ptr);
        if (next == 0)
            return ptrs;
    }
    return {};
}

// Vert/Tri buffer: gzip (first u32 == 0x00088B1F) of comp_size bytes, else raw_size raw bytes.
bool Buffer(Cursor c, std::size_t raw_size, std::size_t comp_size, std::vector<std::uint8_t>& out) {
    if (!c.Need(4))
        return false;
    std::uint32_t head{};
    std::memcpy(&head, c.d.data() + c.at, 4);
    if (head != 559903u) {
        if (!c.Need(raw_size))
            return false;
        out.assign(c.d.begin() + std::ptrdiff_t(c.at),
                   c.d.begin() + std::ptrdiff_t(c.at + raw_size));
        return true;
    }
    if (!c.Need(comp_size))
        return false;
    return Gunzip(c.d.subspan(c.at, comp_size), out);
}

} // namespace

bool ParseMaproom(std::span<const std::uint8_t> file, MaproomModel& out, std::string* err) {
    out = {};
    Cursor c{file};
    if (!c.Need(8) || std::memcmp(file.data(), "MMDL", 4) != 0)
        return Fail(err, "bcmdl: not MMDL");
    c.Skip(4);
    if (c.Get<std::uint32_t>() != 0x003A0001u)
        return Fail(err, "bcmdl: unexpected version");
    std::array<std::uint64_t, 10> toc{};
    for (auto& v : toc)
        v = c.Get<std::uint64_t>();
    if (c.bad)
        return Fail(err, "bcmdl: truncated header");
    const auto vinfo = SubToc(c.At(toc[0]));
    const auto tinfo = SubToc(c.At(toc[1]));
    const auto subs = SubToc(c.At(toc[2]));
    if (vinfo.size() != 1 || tinfo.size() != 1 || subs.size() != 1)
        return Fail(err, "bcmdl: expected one vertex info, tri info and submesh");

    // submesh (TOC2 entry): 19 floats (world AABB, not a transform), -1, tris, verts, count, -1,
    // submesh-info TOC pointer, 3-float vertex offset, -1
    Cursor s = c.At(subs[0]);
    s.Skip(19 * 4 + 4 + 8 + 8 + 4 + 4);
    const auto info_toc = s.Get<std::uint64_t>();
    std::array<double, 3> transform{};
    for (auto& v : transform)
        v = double(s.F32());
    if (s.bad)
        return Fail(err, "bcmdl: truncated submesh");
    const auto infos = SubToc(c.At(info_toc));
    if (infos.empty())
        return Fail(err, "bcmdl: no submesh info");
    Cursor si = c.At(infos[0]);
    const auto skinning = si.Get<std::uint32_t>();
    si.Skip(8); // index offset, index count
    const auto jmap_count = si.Get<std::uint32_t>();
    const auto jmap_at = si.Get<std::uint64_t>();
    Cursor jm = c.At(jmap_at);
    const auto jmap0 = jm.Get<std::uint32_t>();
    if (si.bad || jm.bad || skinning != 0 || jmap_count != 1)
        return Fail(err, "bcmdl: unexpected submesh info");

    // joints: count, -1, TOC, TOC2; each entry: transform ptr, name ptr, parent name ptr, unk
    Cursor ji = c.At(toc[7]);
    ji.Skip(8);
    const auto joints_toc = ji.Get<std::uint64_t>();
    if (ji.bad)
        return Fail(err, "bcmdl: truncated joints");
    struct Joint {
        std::string_view name;
        std::optional<std::string_view> parent;
        std::array<float, 9> t{};
    };
    std::vector<Joint> joints;
    for (const auto ptr : SubToc(c.At(joints_toc))) {
        Cursor je = c.At(ptr);
        const auto t_at = je.Get<std::uint64_t>();
        const auto name_at = je.Get<std::uint64_t>();
        const auto parent_at = je.Get<std::uint64_t>();
        Joint j;
        Cursor nc = c.At(name_at);
        j.name = nc.Str();
        if (parent_at != 0) {
            Cursor pc = c.At(parent_at);
            j.parent = pc.Str();
            if (pc.bad)
                return Fail(err, "bcmdl: bad joint parent");
        }
        Cursor tc = c.At(t_at);
        for (auto& v : j.t)
            v = tc.F32();
        if (je.bad || nc.bad || tc.bad)
            return Fail(err, "bcmdl: bad joint");
        joints.push_back(j);
    }
    if (jmap0 >= joints.size())
        return Fail(err, "bcmdl: jMap out of range");
    // joint world position: own + every ancestor's position, summed in double from 0.0 leaf-first;
    // the baker asserts identity rotation and unit scale.
    std::map<std::string_view, const Joint*> by_name;
    for (const auto& j : joints)
        by_name[j.name] = &j; // a repeated name keeps the last, like the baker's dict
    double jx = 0.0, jy = 0.0;
    std::optional<std::string_view> name = joints[jmap0].name;
    for (int guard = 0; name && guard < 64; ++guard) {
        const auto it = by_name.find(*name);
        if (it == by_name.end())
            return Fail(err, "bcmdl: unknown joint");
        const auto& t = it->second->t;
        if (t[3] != 0.0f || t[4] != 0.0f || t[5] != 0.0f || t[6] != 1.0f || t[7] != 1.0f ||
            t[8] != 1.0f)
            return Fail(err, "bcmdl: joint with rotation or scale");
        jx += double(t[0]);
        jy += double(t[1]);
        name = it->second->parent;
    }
    const double dx = transform[0] + jx, dy = transform[1] + jy;

    // vertex info: unk[3], buffer_size, count, comp_size, buffer ptr, info count, -1, infos
    Cursor vi = c.At(vinfo[0]);
    vi.Skip(12);
    const auto buffer_size = vi.Get<std::uint32_t>();
    const auto count = vi.Get<std::uint32_t>();
    const auto comp_size = vi.Get<std::uint32_t>();
    const auto buffer_at = vi.Get<std::uint64_t>();
    const auto info_count = vi.Get<std::uint32_t>();
    vi.Skip(4);
    std::map<std::uint32_t, std::uint32_t> offset_of; // semantic -> offset (last wins)
    for (std::uint32_t i = 0; i < info_count && !vi.bad; ++i) {
        const auto semantic = vi.Get<std::uint32_t>();
        const auto offset = vi.Get<std::uint32_t>();
        vi.Skip(8);
        offset_of[semantic] = offset;
    }
    std::vector<std::uint8_t> verts;
    if (vi.bad || !Buffer(c.At(buffer_at), buffer_size, comp_size, verts))
        return Fail(err, "bcmdl: bad vertex buffer");
    if (!offset_of.count(0) || !offset_of.count(5))
        return Fail(err, "bcmdl: no position or colour stream");
    const std::size_t pos_at = offset_of[0], col_at = offset_of[5];
    if (pos_at + std::size_t(count) * 12 > verts.size() ||
        col_at + std::size_t(count) * 16 > verts.size())
        return Fail(err, "bcmdl: vertex streams out of range");
    out.pos.resize(count);
    out.col.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        float p[3], col[4];
        std::memcpy(p, verts.data() + pos_at + std::size_t(i) * 12, 12);
        std::memcpy(col, verts.data() + col_at + std::size_t(i) * 16, 16);
        out.pos[i] = {double(p[0]) + dx, double(p[1]) + dy};
        out.col[i] = {double(col[0]), double(col[1]), double(col[2])};
    }

    // tri info: u64 0, u16 2, u16 1, idx_count, comp_size, -1, buffer ptr
    Cursor ti = c.At(tinfo[0]);
    ti.Skip(12);
    const auto idx_count = ti.Get<std::uint32_t>();
    const auto tcomp = ti.Get<std::uint32_t>();
    ti.Skip(4);
    const auto tbuf_at = ti.Get<std::uint64_t>();
    std::vector<std::uint8_t> tb;
    if (ti.bad || !Buffer(c.At(tbuf_at), std::size_t(idx_count) * 2, tcomp, tb))
        return Fail(err, "bcmdl: bad index buffer");
    const bool wide = tb.size() != std::size_t(idx_count) * 2;
    if (tb.size() < std::size_t(idx_count) * (wide ? 4 : 2))
        return Fail(err, "bcmdl: index buffer too small");
    const auto index = [&](std::size_t k) -> std::uint32_t {
        if (wide) {
            std::uint32_t v;
            std::memcpy(&v, tb.data() + k * 4, 4);
            return v;
        }
        std::uint16_t v;
        std::memcpy(&v, tb.data() + k * 2, 2);
        return v;
    };
    for (std::size_t k = 0; k + 2 < idx_count; k += 3) {
        const std::array<std::uint32_t, 3> t{index(k), index(k + 1), index(k + 2)};
        if (t[0] >= count || t[1] >= count || t[2] >= count)
            return Fail(err, "bcmdl: index out of range");
        out.tris.push_back(t);
    }
    return true;
}

bool ParseCameraRects(std::span<const std::uint8_t> file, std::vector<std::array<double, 4>>& out,
                      std::string* err) {
    out.clear();
    Cursor c{file};
    if (!c.Need(8) || std::memcmp(file.data(), "MSCD", 4) != 0)
        return Fail(err, "bmscc: not MSCD");
    c.Skip(4);
    // version 1.16.0: u16 major, u8 minor, u8 patch
    if (c.Get<std::uint16_t>() != 1 || c.Get<std::uint8_t>() != 16 || c.Get<std::uint8_t>() != 0)
        return Fail(err, "bmscc: unexpected version");
    const auto layers = c.Get<std::uint32_t>();
    if (c.bad || layers == 0)
        return Fail(err, "bmscc: no layers");
    c.Str(); // layer name
    const auto entries = c.Get<std::uint32_t>();
    for (std::uint32_t e = 0; e < entries && !c.bad; ++e) {
        for (int k = 0; k < 4; ++k)
            c.Str(); // name, prop1..3
        c.Skip(2); // flag
        const std::string_view type = c.Str();
        if (type != "POLYCOLLECTION2D")
            return Fail(err, "bmscc: a camera that is not a POLYCOLLECTION2D");
        c.Skip(12); // position
        const auto polys = c.Get<std::uint32_t>();
        double minx = 0, miny = 0, maxx = 0, maxy = 0;
        bool any = false;
        for (std::uint32_t p = 0; p < polys && !c.bad; ++p) {
            const auto n = c.Get<std::uint32_t>();
            c.Skip(4); // unk
            if (!c.Need(std::size_t(n) * 12 + 1 + 16))
                break;
            for (std::uint32_t i = 0; i < n; ++i) {
                const double x = double(c.F32()), y = double(c.F32());
                c.Skip(4); // material attribute
                if (n < 3)
                    continue;
                if (!any) {
                    minx = maxx = x;
                    miny = maxy = y;
                    any = true;
                } else {
                    // Python min()/max() keep the first of equal values; for doubles that only
                    // matters for -0.0 vs 0.0, which min/max below also keep first.
                    if (x < minx)
                        minx = x;
                    if (x > maxx)
                        maxx = x;
                    if (y < miny)
                        miny = y;
                    if (y > maxy)
                        maxy = y;
                }
            }
            c.Skip(1 + 16); // loop flag, boundings
        }
        c.Skip(16); // total boundings
        if (c.Get<std::uint8_t>() != 0) { // optional binary search trees
            const auto trees = c.Get<std::uint32_t>();
            c.Skip(std::size_t(trees) * 20);
        }
        if (c.bad)
            break;
        if (any)
            out.push_back({PyRound1(minx), PyRound1(miny), PyRound1(maxx), PyRound1(maxy)});
    }
    if (c.bad)
        return Fail(err, "bmscc: truncated");
    return true;
}

} // namespace dread_mapgen
