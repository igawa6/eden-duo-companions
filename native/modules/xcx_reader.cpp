// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_reader.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>
namespace xcx {
float HeadingFromRaw(float raw) {
    constexpr double Pi = 3.14159265358979323846;
    double h = std::fmod(-(double(raw) + Pi), 2 * Pi);
    if (h <= -Pi)
        h += 2 * Pi;
    else if (h > Pi)
        h -= 2 * Pi;
    return float(h);
}
namespace {
template <class T> T At(const std::uint8_t *p, std::size_t o = 0) {
    T v;
    std::memcpy(&v, p + o, sizeof v);
    return v;
}
bool Pointer(std::uint64_t p) { return p >= 0x10000 && p < 0x10000000000 && !(p & 7); }
struct Memory {
    const EdenDsmodHostApi &h;
    bool Bytes(std::uint64_t p, void *out, std::size_t n) const {
        return p && n && p <= UINT64_MAX - n && h.is_mapped && h.read_memory &&
               h.is_mapped(h.userdata, p, n) && h.read_memory(h.userdata, p, out, n);
    }
    template <class T> bool Get(std::uint64_t p, T &v) const { return Bytes(p, &v, sizeof v); }
};
bool ActorRead(const Memory &m, std::uint64_t table, std::uint32_t handle, int type, Actor &out) {
    out = {};
    out.handle = handle;
    if (!(handle & 0x7ffff))
        return true;
    const auto index = (handle >> 19) & 0xfff;
    const auto e = table + index * 0x18 + (handle & 0x80000000 ? 0x1a038 : 0x10);
    std::array<std::uint8_t, 24> before{}, after{};
    if (!m.Get(e, before) || At<std::uint32_t>(before.data(), 16) != (handle & 0x7ffff))
        return false;
    const auto p = At<std::uint64_t>(before.data());
    std::array<std::uint8_t, 0x2c0> object{};
    if (!Pointer(p) || !m.Get(p, object) || object[0xb4] != type)
        return false;
    out.id = At<std::uint16_t>(object.data(), 0x28c);
    if (type == 2)
        out.name = DecodeActorName(object.data() + NameOffset, NameBytes);
    const auto status = At<std::uint64_t>(object.data(), 0x200);
    std::uint64_t vt{};
    if (!Pointer(status) || !m.Get(status, vt) || vt < m.h.main_base)
        return false;
    constexpr std::array<std::uint64_t, 5> vtables{0x1c31078, 0x1c313d8, 0x1c31748, 0x1c31d30,
                                                   0x1c32150};
    if (std::find(vtables.begin(), vtables.end(), vt - m.h.main_base) == vtables.end() ||
        (type == 4 && vt - m.h.main_base != 0x1c313d8))
        return false;
    // HP/TP (the first 32 bytes) are required; the rest of block 2 (the profile stats) is optional.
    std::array<std::uint8_t, profile::StatsBytes> stats{};
    if (type == 2 && m.Get(status + 0x250, stats))
        out.stats = profile::DecodeStats(stats);
    else if (!m.Bytes(status + 0x250, stats.data(), 32))
        return false;
    out.hp = At<std::int32_t>(stats.data());
    out.max_hp = At<std::int32_t>(stats.data(), 4);
    out.points = At<std::int32_t>(stats.data(), 24);
    out.max_points = At<std::int32_t>(stats.data(), 28);
    if (out.max_hp <= 0 || out.max_hp > 100000000 || out.hp < 0 || out.hp > out.max_hp ||
        out.points < 0 || out.max_points < out.points || out.max_points > 10000000)
        return false;
    out.driven = (object[0xd0] & 0x20) != 0;
    if (const auto state = At<std::uint64_t>(object.data(), 0x1e0); Pointer(state)) {
        std::int32_t v{};
        if (m.Get(state + 0x34, v) && v >= 0 && v < 64)
            out.battle_state = v;
    }
    if (const float raw = At<float>(object.data(), 0x64); std::isfinite(raw) && std::abs(raw) <= 4.f) {
        out.yaw = HeadingFromRaw(raw);
        out.yaw_valid = true;
    }
    for (int i = 0; i < 3; ++i) {
        out.xyz[i] = At<float>(object.data(), 0x50 + 4 * i);
        if (!std::isfinite(out.xyz[i]) || std::abs(out.xyz[i]) > 1000000)
            return false;
    }
    if (type == 4) {
        std::array<float, 2> fuel{};
        if (!m.Get(status + 0x3cf0, fuel) || !std::isfinite(fuel[0]) || !std::isfinite(fuel[1]) ||
            fuel[0] < 0 || fuel[1] <= 0 || fuel[0] > fuel[1] || fuel[1] > 10000000)
            return false;
        out.fuel = fuel[0];
        out.max_fuel = fuel[1];
    }
    if (!m.Get(e, after) || before != after)
        return false;
    out.present = true;
    return true;
}
std::string Text(const std::uint8_t *p, std::size_t n) {
    const auto end = std::find(p, p + n, 0);
    if (end == p + n)
        return {};
    // Preserve UTF-8 bytes for rendering; reject control characters, never walk beyond the field.
    if (std::any_of(p, end, [](auto c) { return c < 32; }))
        return {};
    return {reinterpret_cast<const char *>(p), std::size_t(end - p)};
}
} // namespace
std::string DecodeActorName(const std::uint8_t *field, std::size_t n) {
    // A NUL-terminated UTF-8 string inside the field; the game leaves stale bytes after the NUL.
    const auto end = std::find(field, field + n, 0);
    if (end == field + n || end == field)
        return {};
    std::size_t i = 0;
    const std::size_t len = end - field;
    while (i < len) {
        const auto c = field[i];
        const std::size_t extra =
            c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (c < 32 || c == 127 || extra == 99 || i + extra >= len)
            return {};
        for (std::size_t k = 1; k <= extra; ++k)
            if ((field[i + k] & 0xc0) != 0x80)
                return {};
        i += extra + 1;
    }
    return {reinterpret_cast<const char *>(field), len};
}
AvatarLook DecodeAvatarBlock(std::span<const std::uint8_t> block, std::string_view actor_name) {
    AvatarLook out;
    if (block.size() != AvatarBlockSize || actor_name.empty() || actor_name.size() >= 0x40)
        return out;
    const auto *name = block.data();
    const auto *end = std::find(name, name + 0x40, 0);
    if (end == name + 0x40 || std::string_view(reinterpret_cast<const char *>(name),
                                               std::size_t(end - name)) != actor_name)
        return out;
    if (At<std::uint32_t>(block.data(), 0x40) != actor_name.size() || block[0x78] != 1 || block[0x79] != 0)
        return out;
    const auto face = At<std::uint16_t>(block.data(), 0x44), hair = At<std::uint16_t>(block.data(), 0x46),
               sex = At<std::uint16_t>(block.data(), 0x52), c0 = At<std::uint16_t>(block.data(), 0x58),
               c5 = At<std::uint16_t>(block.data(), 0x62);
    if (!face || !hair || hair > 4000 || (sex != 1 && sex != 2) || c0 > 4000 || c5 > 4000)
        return out;
    out = {true, face, hair, sex, c0, c5};
    return out;
}
bool SupportsBuild(std::string_view id) {
    if (id.size() != Build.size())
        return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        auto c = id[i];
        if (c >= 'a' && c <= 'f')
            c -= 32;
        if (c != Build[i])
            return false;
    }
    return true;
}
bool SupportsBuild(const std::uint8_t *id) {
    if (!id)
        return false;
    constexpr char hex[] = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 32; ++i) {
        s += hex[id[i] >> 4];
        s += hex[id[i] & 15];
    }
    return SupportsBuild(s);
}
Snapshot Read(const EdenDsmodHostApi &h) {
    Snapshot s;
    if (!dsmod_sdk::HostAbiMatches(&h) || h.title_id != TitleId || !SupportsBuild(h.build_id) ||
        !h.main_base)
        return s;
    const Memory m{h};
    std::uint64_t table{}, root{}, zone{};
    std::array<std::uint8_t, 0x60> before{}, after{};
    if (!m.Get(h.main_base + 0x1d55130, table) || !Pointer(table) ||
        !m.Get(h.main_base + 0x1df58d0, before) || !m.Get(h.main_base + 0x1df5968, zone) ||
        !Pointer(zone))
        return s;
    std::array<std::uint8_t, 0x100> z{};
    if (!m.Get(zone, z))
        return s;
    s.zone = At<std::uint32_t>(z.data(), 0xac);
    if (!s.zone)
        return {}; // +a8 is a distinct world identifier (16 in NLA), not zone1401.
    s.area = Text(z.data() + 0xb8, 0x48);
    s.nla = s.zone == 1401;
    // Saved/UI records are optional. No stale record is substituted when a lookup fails.
    const bool have_root = m.Get(h.main_base + 0x1dc1408, root) && Pointer(root);
    if (have_root) {
        std::vector<std::uint8_t> flags(FlagBytes);
        if (m.Bytes(root + FlagAreaOffset, flags.data(), flags.size()))
            s.flags = std::move(flags);
    }
    struct Assignment {
        std::uint64_t address;
        std::uint8_t index;
    };
    std::vector<Assignment> assignments;
    int count = 0;
    for (int slot = 0; slot < 4; ++slot) {
        auto &member = s.party[slot];
        const auto off = 0xc + 0x14 * slot;
        if (!ActorRead(m, table, At<std::uint32_t>(before.data(), off), 2, member.pc) ||
            !ActorRead(m, table, At<std::uint32_t>(before.data(), off + 4), 4, member.skell))
            return {};
        if (!member.pc.present) {
            if (member.skell.present)
                return {};
            continue;
        }
        if (member.pc.id > 22)
            return {}; // Other actor kinds/IDs need their own mapping.
        for (int j = 0; j < slot; ++j)
            if (s.party[j].pc.present && s.party[j].pc.id == member.pc.id)
                return {};
        ++count;
        member.controlled = before[off + 9] != 0;
        member.mounted = member.skell.present && before[off + 10] == 0;
        if (member.controlled) {
            if (s.controlled != -1)
                return {};
            s.controlled = slot;
        }
        if (!have_root)
            continue;
        {
            std::array<std::uint8_t, profile::RecordBytes> record{};
            if (m.Get(root + profile::RecordOffset + member.pc.id * profile::RecordStride, record))
                member.profile = profile::DecodeRecord(record, member.pc.id == 0 ? std::string_view(member.pc.name)
                                                                                  : std::string_view{});
        }
        std::uint8_t index{};
        const auto address = root + 0x8ab + member.pc.id * 0x7f8;
        if (!m.Get(address, index))
            continue;
        assignments.push_back({address, index});
        member.owned_index = index;
        if (index > 59)
            continue;
        std::array<std::uint8_t, 0x400> record{};
        if (!m.Get(root + 0xc368 + index * 0x400, record))
            continue;
        const auto frame = At<std::uint16_t>(record.data(), 0x22c);
        const auto mode = record[0x3f4];
        if ((mode != 4 && mode != 6 && mode != 7) || !member.skell.present ||
            frame != member.skell.id)
            continue;
        member.skell_name = Text(record.data(), 32);
        member.frame = frame;
        member.insurance = record[0x3f8];
        std::memcpy(member.paint.data(), record.data() + 0x364, sizeof member.paint);
        for (std::size_t b = 0; b < member.weapons.size(); ++b) {
            const auto item = At<std::uint16_t>(record.data(), 0x24 + 0x34 * b);
            member.weapons[b] = item == 0xffff ? 0 : item;
        }
        member.owned = true;
    }
    if (!count || s.controlled < 0 || !m.Get(h.main_base + 0x1df58d0, after) || before != after)
        return {};
    // The avatar's own record (appearance for the portrait). Optional: a failed or mismatching read
    // only leaves the portrait to the guarded fallback, never the snapshot.
    std::array<std::uint8_t, AvatarBlockSize> avatar_block{};
    bool have_avatar = false;
    if (have_root)
        for (const auto &member : s.party)
            if (member.pc.present && member.pc.id == 0) {
                have_avatar = m.Get(root + AvatarBlockOffset, avatar_block);
                if (have_avatar)
                    s.avatar = DecodeAvatarBlock(avatar_block, member.pc.name);
                break;
            }
    std::uint64_t table_after{}, zone_after{}, root_after{};
    std::uint32_t zone_id_after{};
    if (!m.Get(h.main_base + 0x1d55130, table_after) || table_after != table ||
        !m.Get(h.main_base + 0x1df5968, zone_after) || zone_after != zone ||
        !m.Get(zone + 0xac, zone_id_after) || zone_id_after != s.zone)
        return {};
    if (have_root) {
        if (!m.Get(h.main_base + 0x1dc1408, root_after) || root_after != root)
            return {};
        for (auto a : assignments) {
            std::uint8_t index{};
            if (!m.Get(a.address, index) || index != a.index)
                return {};
        }
        if (have_avatar) {
            std::array<std::uint8_t, AvatarBlockSize> again{};
            if (!m.Get(root + AvatarBlockOffset, again) || again != avatar_block)
                s.avatar = {}; // changing while read: no portrait this time
        }
    }
    // The HUD rows (diagnostics for the Battle page's HP and GP checks). Optional.
    if (std::uint64_t hud{}; m.Get(h.main_base + 0x1df5978, hud) && Pointer(hud))
        for (int slot = 0; slot < 4; ++slot) {
            std::array<std::int32_t, 5> row{};
            if (m.Get(hud + 0x1dfb4 + 0xdd0 * slot, row))
                s.hud[slot] = {row[0], row[1], row[2], row[3], row[4]};
        }
    s.valid = true;
    return s;
}
} // namespace xcx
