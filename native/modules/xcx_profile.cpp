// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_profile.h"
#include "xcx_bdat.h"
#include <algorithm>
#include <cstring>
namespace xcx::profile {
namespace {
template <class T> T Le(std::span<const std::uint8_t> b, std::size_t o) {
    T v{};
    if (o + sizeof v <= b.size())
        std::memcpy(&v, b.data() + o, sizeof v);
    return v;
}
// common.bdat tables (by name hash, PROFILE-DATA s4) and their columns (murmur3 name hashes).
constexpr std::uint32_t TableWeapon = 0x6820913c, TableArmor = 0x69a6c37f, TableEffect = 0xb6c0c2f8,
                        TableClass = 0x7cff813b;
constexpr std::uint32_t ColName = 0x25efa387, ColRarity = 0x99df46ca, ColLevel = 0x96909221,
                        ColAttack = 0x1e689e67, ColAccuracy = 0xa2f7edf6, ColMaxRank = 0xd94a52f4;
// common_ms.bdat text tables (string column = the table's first string column).
constexpr std::uint32_t TextWeapon = 0x1d7d9d73, TextArmor = 0xf6c72a5b, TextEffect = 0xe052d48f,
                        TextClass = 0xfb8ed214;
bool Text(std::span<const std::uint8_t> ms, std::uint32_t hash, std::uint32_t &base, std::vector<std::string> &out) {
    bdat::Table t;
    if (!t.OpenByName(ms, hash))
        return false;
    int column = -1;
    for (unsigned c = 0; c < t.cols && column < 0; ++c)
        if (t.type[c] == 7)
            column = int(c);
    if (column < 0)
        return false;
    base = t.base;
    out.clear();
    out.reserve(t.rows);
    for (unsigned r = 0; r < t.rows; ++r)
        out.emplace_back(t.String(t.base + r, column));
    return true;
}
} // namespace
Record DecodeRecord(std::span<const std::uint8_t> b, std::string_view expect_name) {
    Record r;
    if (b.size() < RecordBytes)
        return r;
    const auto *name = reinterpret_cast<const char *>(b.data());
    const auto *end = static_cast<const char *>(std::memchr(name, 0, 0x40));
    if (!end || end == name)
        return r;
    if (!expect_name.empty() && std::string_view(name, std::size_t(end - name)) != expect_name)
        return r;
    r.level = b[0x7a];
    r.class_id = b[0x2f0];
    r.rank = Le<std::uint16_t>(b, 0x2f4);
    if (r.level < 1 || r.level > 99 || r.class_id < 1 || r.class_id > 63 || r.rank < 1 || r.rank > 30)
        return r;
    for (std::size_t k = 0; k < GearSlots; ++k) {
        const auto o = GearOffset + GearStride * k;
        auto &g = r.gear[k];
        g.item = Le<std::uint16_t>(b, o);
        if (!g.item)
            continue;
        if ((Le<std::uint16_t>(b, o + 4) & 0x3f) != GearCategory[k])
            return r;
        for (int e = 0; e < 3; ++e)
            g.effects[e] = Le<std::uint16_t>(b, o + 8 + 2 * e);
    }
    r.valid = true;
    return r;
}
Stats DecodeStats(std::span<const std::uint8_t> b) {
    Stats s;
    if (b.size() < StatsBytes)
        return s;
    const auto at = [&](std::size_t o) { return Le<std::int32_t>(b, o); };
    s.melee = at(0x20);
    s.ranged = at(0x24);
    s.potential = at(0x28);
    s.defense = at(0x2c);
    s.melee_acc = at(0x30);
    s.ranged_acc = at(0x34);
    s.evasion = at(0x38);
    for (int i = 0; i < 6; ++i)
        s.res[i] = at(0x40 + 4 * i);
    const auto sane = [](std::int32_t v, std::int32_t lo, std::int32_t hi) { return v >= lo && v <= hi; };
    s.valid = sane(s.melee, 0, 99999) && sane(s.ranged, 0, 99999) && sane(s.potential, 0, 99999) &&
              sane(s.defense, 0, 99999) && sane(s.melee_acc, 0, 99999) && sane(s.ranged_acc, 0, 99999) &&
              sane(s.evasion, 0, 99999) &&
              std::all_of(s.res.begin(), s.res.end(), [&](std::int32_t v) { return sane(v, -1000, 1000); });
    return s;
}
bool Names::Load(std::span<const std::uint8_t> common, std::span<const std::uint8_t> ms) {
    ready = false;
    const auto items = [&](std::uint32_t hash, std::vector<Row> &rows, std::uint32_t &base) {
        bdat::Table t;
        if (!t.OpenByName(common, hash))
            return false;
        const int name = t.Column(ColName), rarity = t.Column(ColRarity), level = t.Column(ColLevel);
        const int attack = t.Column(ColAttack), accuracy = t.Column(ColAccuracy);
        if (name < 0 || rarity < 0 || level < 0)
            return false;
        base = t.base;
        rows.assign(t.rows, {});
        for (unsigned r = 0; r < t.rows; ++r) {
            const unsigned id = t.base + r;
            rows[r] = {std::uint16_t(t.Int(id, name)), std::uint16_t(t.Int(id, level)),
                       std::uint8_t(t.Int(id, rarity)), std::int16_t(attack < 0 ? 0 : t.Int(id, attack)),
                       std::int16_t(accuracy < 0 ? 0 : t.Int(id, accuracy))};
        }
        return true;
    };
    if (!items(TableWeapon, weapons, weapon_base) || !items(TableArmor, armor, armor_base))
        return false;
    {
        bdat::Table t;
        if (!t.OpenByName(common, TableEffect) || t.Column(ColName) < 0)
            return false;
        effect_base = t.base;
        effects.assign(t.rows, 0);
        for (unsigned r = 0; r < t.rows; ++r)
            effects[r] = std::uint16_t(t.Int(t.base + r, t.Column(ColName)));
    }
    {
        bdat::Table t;
        if (!t.OpenByName(common, TableClass) || t.Column(ColName) < 0 || t.Column(ColMaxRank) < 0)
            return false;
        class_base = t.base;
        classes.assign(t.rows, {});
        for (unsigned r = 0; r < t.rows; ++r)
            classes[r] = {std::uint16_t(t.Int(t.base + r, t.Column(ColName))),
                          std::uint8_t(t.Int(t.base + r, t.Column(ColMaxRank)))};
    }
    if (!Text(ms, TextWeapon, weapon_text.base, weapon_text.text) ||
        !Text(ms, TextArmor, armor_text.base, armor_text.text) ||
        !Text(ms, TextEffect, effect_text.base, effect_text.text) ||
        !Text(ms, TextClass, class_text.base, class_text.text))
        return false;
    ready = true;
    return true;
}
ItemInfo Names::Weapon(unsigned id) const {
    ItemInfo i;
    if (id < weapon_base || id - weapon_base >= weapons.size())
        return i;
    const auto &r = weapons[id - weapon_base];
    i.name = std::string(weapon_text.At(r.name));
    i.known = !i.name.empty();
    i.level = r.level;
    i.rarity = r.rarity;
    i.attack = r.attack;
    i.accuracy = r.accuracy;
    return i;
}
ItemInfo Names::Armor(unsigned id) const {
    ItemInfo i;
    if (id < armor_base || id - armor_base >= armor.size())
        return i;
    const auto &r = armor[id - armor_base];
    i.name = std::string(armor_text.At(r.name));
    i.known = !i.name.empty();
    i.level = r.level;
    i.rarity = r.rarity;
    return i;
}
std::string Names::Effect(unsigned id) const {
    if (id < effect_base || id - effect_base >= effects.size())
        return {};
    return std::string(effect_text.At(effects[id - effect_base]));
}
std::string Names::Class(unsigned id) const {
    if (id < class_base || id - class_base >= classes.size())
        return {};
    return std::string(class_text.At(classes[id - class_base].name));
}
int Names::MaxRank(unsigned id) const {
    if (id < class_base || id - class_base >= classes.size())
        return 0;
    return classes[id - class_base].max_rank;
}
std::string RankText(int rank, int max_rank) {
    if (rank <= 0)
        return "--";
    return max_rank > 0 && rank >= max_rank ? std::string("MAX") : std::to_string(rank);
}
} // namespace xcx::profile
