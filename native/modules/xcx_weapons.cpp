// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_weapons.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
namespace xcx {
namespace {
using Span = std::span<const std::uint8_t>;
void Need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
// A read-only view of one modern-BDAT table of /bdat/common.bdat (identity checked by hash).
class Table {
    Span b;
    std::size_t col_at{}, row_at{}, str_at{}, str_len{};
    unsigned cols{}, rows{}, base{}, stride{};
    std::array<unsigned, 128> offset{}, type{}, name{};
    std::uint32_t U32(std::size_t p) const {
        Need(p + 4 <= b.size(), "XCX bdat truncated");
        std::uint32_t v;
        std::memcpy(&v, b.data() + p, 4);
        return v;
    }
    std::uint32_t U16(std::size_t p) const {
        Need(p + 2 <= b.size(), "XCX bdat truncated");
        std::uint16_t v;
        std::memcpy(&v, b.data() + p, 2);
        return v;
    }

  public:
    Table(Span bytes, unsigned index, std::uint32_t identity) : b(bytes) {
        Need(U32(0) == 0x54414442 && U32(4) == 0x01001004 && U32(8) > index && U32(12) == b.size(),
             "XCX bdat header");
        const std::size_t t = U32(16 + std::size_t(index) * 4);
        Need(U32(t) == 0x54414442 && U32(t + 4) == 0x3004, "XCX bdat table header");
        cols = U32(t + 8);
        rows = U32(t + 12);
        base = U32(t + 16);
        col_at = t + U32(t + 24);
        row_at = t + U32(t + 32);
        stride = U32(t + 36);
        str_at = t + U32(t + 40);
        str_len = U32(t + 44);
        Need(cols > 0 && cols <= 128 && rows <= 65536 && stride && stride <= 1024 &&
                 row_at + std::uint64_t(rows) * stride <= b.size() &&
                 str_at + str_len <= b.size() && col_at + cols * 3ull <= b.size() &&
                 U32(str_at + 1) == identity,
             "XCX wrong bdat table");
        constexpr unsigned widths[]{0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 4, 2};
        unsigned at = 0;
        for (unsigned c = 0; c < cols; ++c) {
            type[c] = b[col_at + c * 3];
            Need(type[c] > 0 && type[c] < std::size(widths), "XCX bdat column type");
            name[c] = U32(str_at + U16(col_at + c * 3 + 1));
            offset[c] = at;
            at += widths[type[c]];
        }
        Need(at == stride, "XCX bdat row stride");
    }
    int Column(std::uint32_t hash) const {
        for (unsigned c = 0; c < cols; ++c)
            if (name[c] == hash)
                return int(c);
        return -1;
    }
    bool HasRow(unsigned id) const { return id >= base && id - base < rows; }
    std::size_t Cell(unsigned id, int col) const {
        Need(col >= 0 && HasRow(id), "XCX bdat cell");
        return row_at + std::size_t(id - base) * stride + offset[col];
    }
    unsigned Number(unsigned id, int col) const {
        const auto p = Cell(id, col);
        switch (type[col]) {
        case 1: case 4: case 10: return b[p];
        case 2: case 5: case 13: return U16(p);
        default: return U32(p);
        }
    }
    std::string Text(unsigned id, int col) const {
        Need(type[col] == 7 || type[col] == 11, "XCX bdat not a string");
        const auto s = std::size_t(U32(Cell(id, col)));
        Need(s < str_len, "XCX bdat string offset");
        const auto end = std::find(b.begin() + str_at + s, b.begin() + str_at + str_len, 0);
        Need(end != b.begin() + str_at + str_len, "XCX bdat string end");
        std::string out(b.begin() + str_at + s, end);
        Need(out.size() < 64 && std::all_of(out.begin(), out.end(),
                                            [](char c) { return c >= 32 && c < 127; }),
             "XCX bdat string content");
        return out;
    }
};
std::string Safe(std::string s) { return s; }
} // namespace
WeaponItems WeaponItemsFromRecord(Span record) {
    WeaponItems items{};
    if (record.size() != 0x400)
        return items;
    for (unsigned blk = 0; blk < items.size(); ++blk) {
        std::uint16_t v;
        std::memcpy(&v, record.data() + 0x24 + 0x34 * blk, 2);
        items[blk] = v == 0xffff ? 0 : v;
    }
    return items;
}
std::map<std::string, std::string> ParseLocators(Span d) {
    // CSVB: magic, u16 record count, u16, u16 gap to the strings (at +8); u16 widths[count]; then 16-byte
    // records of (u16 name offset, u16 bone offset, six i16); strings follow the gap. Offsets are
    // relative to the string base.
    std::map<std::string, std::string> out;
    if (d.size() < 0x10 || std::memcmp(d.data(), "CSVB", 4))
        return out;
    const auto u16 = [&](std::size_t p) -> unsigned {
        if (p + 2 > d.size())
            return 0xffffffffu;
        return unsigned(d[p]) | unsigned(d[p + 1]) << 8;
    };
    const unsigned count = u16(4), gap = u16(8);
    if (count == 0 || count > 64)
        return out;
    const std::size_t rec = 0x10 + 2 * std::size_t(count), strings = rec + 16 * std::size_t(count) + gap;
    if (strings >= d.size() || rec + 16 * std::size_t(count) > d.size())
        return {};
    const auto str = [&](unsigned off) -> std::string {
        if (strings + off >= d.size())
            return {};
        const auto s = d.begin() + strings + off;
        const auto e = std::find(s, d.end(), 0);
        if (e == d.end() || e - s > 63)
            return {};
        std::string r(s, e);
        return std::all_of(r.begin(), r.end(), [](char c) { return c >= 32 && c < 127; }) ? r
                                                                                           : std::string();
    };
    for (unsigned i = 0; i < count; ++i) {
        const auto name = str(u16(rec + 16 * std::size_t(i))), bone = str(u16(rec + 16 * std::size_t(i) + 2));
        if (name.empty() || bone.empty())
            return {};
        out[name] = bone;
    }
    return out;
}
std::uint32_t BoneHash(std::string_view name) {
    // murmur3_32, seed 0 (the bone-name hashes of BC/ANIM track bindings)
    const auto *p = reinterpret_cast<const std::uint8_t *>(name.data());
    const std::size_t n = name.size();
    std::uint32_t h = 0;
    const auto mix = [](std::uint32_t k) {
        k *= 0xcc9e2d51u;
        k = k << 15 | k >> 17;
        return k * 0x1b873593u;
    };
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        std::uint32_t k;
        std::memcpy(&k, p + i, 4);
        h ^= mix(k);
        h = h << 13 | h >> 19;
        h = h * 5 + 0xe6546b64u;
    }
    std::uint32_t k = 0;
    switch (n & 3) {
    case 3: k ^= std::uint32_t(p[i + 2]) << 16; [[fallthrough]];
    case 2: k ^= std::uint32_t(p[i + 1]) << 8; [[fallthrough]];
    case 1: k ^= p[i]; h ^= mix(k);
    }
    h ^= std::uint32_t(n);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}
BonePose SampleAnimation(Span sar, std::string_view prefix, float frame) {
    // SAR1: "1RAS", size, version, count, entry table, data; entries of 64 bytes (offset, size,
    // name hash, name[52]). Each entry is a BC file holding one ANIM block: binding (u64 at +0x28) ->
    // animation (binding+0x18) and the extra binding (binding+0x58) whose list at +0x20 has one bone
    // hash per track. Packed cubic data (animation type 3) at animation+0x48: tracks of three
    // (first key, first curve, end key) subtracks, cubic vec4 coefficients (3 per translation/scale
    // curve, 4 per rotation curve), u16 start frames. Checked against xc3_model_py sampling.
    BonePose out;
    const auto u32 = [](Span d, std::uint64_t p) -> std::uint64_t {
        if (p + 4 > d.size())
            throw std::runtime_error("XCX anim truncated");
        std::uint32_t v;
        std::memcpy(&v, d.data() + p, 4);
        return v;
    };
    try {
        if (sar.size() < 0x20 || std::memcmp(sar.data(), "1RAS", 4))
            return out;
        const auto count = u32(sar, 12), table = u32(sar, 16);
        Need(count <= 4096 && table + count * 64 <= sar.size(), "XCX sar table");
        Span b;
        for (std::uint64_t i = 0; i < count && b.empty(); ++i) {
            const auto e = table + i * 64;
            const char *name = reinterpret_cast<const char *>(sar.data() + e + 12);
            const std::string_view nm(name, strnlen(name, 52));
            if (!nm.starts_with(prefix))
                continue;
            const auto off = u32(sar, e), size = u32(sar, e + 4);
            Need(off + size <= sar.size() && size >= 0x40, "XCX sar entry");
            b = sar.subspan(off, size);
        }
        if (b.empty() || std::memcmp(b.data(), "BC", 2) || std::memcmp(b.data() + 0x24, "ANIM", 4))
            return out;
        const auto binding = u32(b, 0x28), anim = u32(b, binding + 0x18), extra = u32(b, binding + 0x58);
        Need(anim + 0x88 <= b.size() && b[anim + 0x20] == 3, "XCX anim not packed cubic");
        const auto hashes = u32(b, extra + 0x20), hash_count = u32(b, extra + 0x28);
        const auto tracks = u32(b, anim + 0x48), track_count = u32(b, anim + 0x50);
        const auto vecs = u32(b, anim + 0x58), vec_count = u32(b, anim + 0x60);
        const auto quats = u32(b, anim + 0x68), quat_count = u32(b, anim + 0x70);
        const auto keys = u32(b, anim + 0x78), key_count = u32(b, anim + 0x80);
        Need(track_count <= 1024 && hash_count >= track_count &&
                 hashes + hash_count * 4 <= b.size() && tracks + track_count * 36 <= b.size() &&
                 vecs + vec_count * 16 <= b.size() && quats + quat_count * 16 <= b.size() &&
                 keys + key_count * 2 <= b.size(),
             "XCX anim lists");
        const auto key = [&](std::uint64_t k) {
            Need(k < key_count, "XCX anim key");
            std::uint16_t v;
            std::memcpy(&v, b.data() + keys + k * 2, 2);
            return float(v);
        };
        const auto coef = [&](std::uint64_t list, std::uint64_t list_count, std::uint64_t row, float t) {
            Need(row < list_count, "XCX anim curve");
            float c[4];
            std::memcpy(c, b.data() + list + row * 16, 16);
            const float v = ((c[0] * t + c[1]) * t + c[2]) * t + c[3];
            Need(std::isfinite(v) && std::abs(v) < 1e5f, "XCX anim value");
            return v;
        };
        for (std::uint64_t i = 0; i < track_count; ++i) {
            BoneTRS trs;
            for (int sub = 0; sub < 3; ++sub) {
                const auto at = tracks + i * 36 + std::uint64_t(sub) * 12;
                const auto first = u32(b, at), curve = u32(b, at + 4), end = u32(b, at + 8);
                Need(first < end && end <= key_count, "XCX anim subtrack");
                auto k = first;
                while (k + 1 < end && key(k + 1) <= frame)
                    ++k;
                const float t = frame - key(k);
                const int width = sub == 1 ? 4 : 3;
                const auto row = curve + (k - first) * width;
                for (int c = 0; c < width; ++c) {
                    if (sub == 0)
                        trs.t[c] = coef(vecs, vec_count, row + c, t);
                    else if (sub == 1)
                        trs.q[c] = coef(quats, quat_count, row + c, t);
                    else
                        trs.s[c] = coef(vecs, vec_count, row + c, t);
                }
            }
            out[std::uint32_t(u32(b, hashes + i * 4))] = trs;
        }
    } catch (const std::runtime_error &) {
        return {};
    }
    return out;
}
std::vector<RigBone> ParseRig(Span d) { return PoseRig(d, {}); }
std::vector<RigBone> PoseRig(Span d, const BonePose &pose) {
    const auto u32 = [&](std::size_t p) -> std::uint64_t {
        if (p + 4 > d.size())
            return ~0ull;
        std::uint32_t v;
        std::memcpy(&v, d.data() + p, 4);
        return v;
    };
    if (d.size() < 0x80 || std::memcmp(d.data(), "BC\0\0", 4) || u32(0x24) != 0x4c454b53)
        return {};
    const auto parents = u32(0x50), names = u32(0x60), transforms = u32(0x70), count = u32(0x68);
    if (count == 0 || count > 512 || u32(0x58) != count || u32(0x78) != count ||
        parents + count * 2 > d.size() || names + count * 16 > d.size() ||
        transforms + count * 48 > d.size())
        return {};
    std::vector<RigBone> bones(count);
    std::vector<std::array<float, 12>> local(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto off = u32(names + i * 16);
        if (off >= d.size())
            return {};
        const auto s = d.begin() + off;
        const auto e = std::find(s, d.end(), 0);
        if (e == d.end() || e == s || e - s > 63)
            return {};
        bones[i].name.assign(s, e);
        if (!std::all_of(bones[i].name.begin(), bones[i].name.end(),
                         [](char c) { return c >= 32 && c < 127; }))
            return {};
        std::int16_t p;
        std::memcpy(&p, d.data() + parents + i * 2, 2);
        if (p < -1 || p >= int(count) || p == int(i))
            return {};
        bones[i].parent = p;
        std::memcpy(local[i].data(), d.data() + transforms + i * 48, 48);
        for (const auto f : local[i])
            if (!std::isfinite(f) || std::abs(f) > 1e5f)
                return {};
    }
    // Animated bones take translation, rotation and scale from the pose (the rest scale stays
    // ignored, as before, so an empty pose gives exactly the old bind-pose transforms).
    std::vector<char> scaled(count, 0);
    if (!pose.empty())
        for (std::size_t i = 0; i < count; ++i)
            if (const auto it = pose.find(BoneHash(bones[i].name)); it != pose.end()) {
                const auto &a = it->second;
                local[i] = {a.t[0], a.t[1], a.t[2], 0,       a.q[0], a.q[1],
                            a.q[2], a.q[3], a.s[0], a.s[1], a.s[2], 0};
                scaled[i] = 1;
            }
    // Compose world transforms (parents may come later in the array: resolve with a depth guard).
    std::vector<char> done(count, 0);
    const std::function<bool(std::size_t, int)> world = [&](std::size_t i, int depth) -> bool {
        if (done[i])
            return true;
        if (depth > 128)
            return false;
        const auto &l = local[i];
        const float x = l[4], y = l[5], z = l[6], w = l[7];
        const float n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n < 1e-3f)
            return false;
        const float qx = x / n, qy = y / n, qz = z / n, qw = w / n;
        std::array<float, 9> rl{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw),
                                2 * (qx * qz + qy * qw),     2 * (qx * qy + qz * qw),
                                1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
                                2 * (qx * qz - qy * qw),     2 * (qy * qz + qx * qw),
                                1 - 2 * (qx * qx + qy * qy)};
        if (scaled[i])
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    rl[r * 3 + c] *= l[8 + c];
        auto &b = bones[i];
        if (b.parent < 0) {
            b.rotation = rl;
            b.position = {l[0], l[1], l[2]};
        } else {
            if (!world(std::size_t(b.parent), depth + 1))
                return false;
            const auto &p = bones[std::size_t(b.parent)];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c)
                    b.rotation[r * 3 + c] = p.rotation[r * 3] * rl[c] + p.rotation[r * 3 + 1] * rl[3 + c] +
                                            p.rotation[r * 3 + 2] * rl[6 + c];
                b.position[r] = p.rotation[r * 3] * l[0] + p.rotation[r * 3 + 1] * l[1] +
                                p.rotation[r * 3 + 2] * l[2] + p.position[r];
            }
        }
        done[i] = 1;
        return true;
    };
    for (std::size_t i = 0; i < count; ++i)
        if (!world(i, 0))
            return {};
    return bones;
}
std::vector<RigBone> RestPoseRig(Archive &archive, const EdenDsmodHostApi &host, std::string_view dir,
                                 std::string_view name, std::string_view entry_prefix) {
    const bool safe = (dir == "dl" || dir == "wd") && !name.empty() && name.size() <= 16 &&
                      std::all_of(name.begin(), name.end(), [](char c) {
                          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
                      });
    if (!safe)
        return {};
    try {
        BonePose pose;
        {
            const auto sar =
                archive.Read(host, "/hkc/" + std::string(dir) + "/" + std::string(name) + ".sar");
            pose = SampleAnimation(sar, entry_prefix, 0.f);
        } // the animation archive (megabytes) is released before the rig is read
        if (pose.empty())
            return {};
        return PoseRig(
            archive.Read(host, "/chr/" + std::string(dir) + "/" + std::string(name) + "_rig.skl"), pose);
    } catch (const std::runtime_error &) {
        return {};
    }
}
std::vector<WeaponMount> ResolveWeapons(Archive &archive, const EdenDsmodHostApi &host,
                                        const FrameModel &frame, const WeaponItems &items) {
    std::vector<WeaponMount> out;
    if (std::all_of(items.begin(), items.end(), [](auto v) { return v == 0; }))
        return out;
    Need(frame.resource.size() == 8 && frame.resource.starts_with("dl"), "XCX weapon frame");
    const auto bdat = archive.Read(host, "/bdat/common.bdat");
    const Table weapons(bdat, 695, 0x36797290), resources(bdat, 598, 0xc6653533);
    const int lcol = weapons.Column(0x2f5e92a4), rcol = weapons.Column(0x46a0d1c6);
    const int res = resources.Column(0x5ba4bc44), motion = resources.Column(0xbbe83d29),
              in_c = resources.Column(0xa05cbcb6), in_w = resources.Column(0x914a2475);
    Need(lcol >= 0 && rcol >= 0 && res >= 0 && motion >= 0 && in_c >= 0 && in_w >= 0,
         "XCX weapon table schema");
    const auto skell_locators = ParseLocators(archive.Read(host, "/chr/dl/" + frame.resource + ".wimtp"));
    const auto rig = ParseRig(archive.Read(host, "/chr/dl/" + frame.resource + "_rig.skl"));
    if (rig.empty() || skell_locators.empty())
        return out; // no readable mount data for this frame: no weapons drawn
    for (unsigned blk = 0; blk < items.size(); ++blk) {
        const unsigned item = items[blk];
        if (!item || !weapons.HasRow(item))
            continue;
        const unsigned rsc = weapons.Number(item, blk % 2 == 0 ? lcol : rcol);
        if (!rsc || !resources.HasRow(rsc))
            continue;
        try {
            auto resource = resources.Text(rsc, res); // "wd/wd0345012l"
            if (!resource.starts_with("wd/"))
                continue;
            resource.erase(0, 3);
            const auto locator = resources.Text(rsc, in_c);
            const auto bone = skell_locators.find(locator);
            if (bone == skell_locators.end())
                continue; // this frame has no such mount: nothing is guessed
            WeaponMount m;
            m.resource = resource;
            m.skell_bone = bone->second;
            m.block = blk;
            m.motion = resources.Text(rsc, motion).substr(3);
            std::size_t at = rig.size();
            for (std::size_t i = 0; i < rig.size(); ++i)
                if (rig[i].name == bone->second)
                    at = i;
            if (at == rig.size())
                continue; // mount bone not in the rig: nothing is guessed
            m.mount_rotation = rig[at].rotation;
            m.mount_position = rig[at].position;
            for (int hop = 0, i = int(at); i >= 0 && hop < 128; ++hop, i = rig[std::size_t(i)].parent)
                if (rig[std::size_t(i)].name == "JU_backD_L" || rig[std::size_t(i)].name == "JU_backD_R") {
                    m.fold_side = rig[std::size_t(i)].name.back() == 'L' ? 1 : 2;
                    break;
                }
            const auto weapon_locators =
                ParseLocators(archive.Read(host, "/chr/wd/" + resources.Text(rsc, motion).substr(3) + ".wimtp"));
            const auto w = weapon_locators.find(resources.Text(rsc, in_w));
            if (w != weapon_locators.end())
                m.weapon_bone = w->second;
            out.push_back(std::move(m));
        } catch (const std::runtime_error &) {
            continue; // an unreadable weapon row is skipped, the rest still draw
        }
    }
    (void)Safe;
    return out;
}
} // namespace xcx
