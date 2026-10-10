// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Brilliant Diamond / Shining Pearl in-RAM party Pokémon (Pml.PokePara.CoreParam):
// m_coreData is the 0x148-byte PK8-style block, encrypted with the LCG keyed by the encryption
// constant and shuffled in four 0x50-byte blocks; m_calcData is 0x10 bytes encrypted with the
// same LCG (level, max HP and the five computed stats). Verified against the native summary
// screen on BD 1.3.0 (Garchomp Lv. 72: 252 HP, 266 Atk, 140 Def, 159 Spe, 185 SpA, 139 SpD).

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace lp_pk8 {

constexpr std::size_t CoreSize = 0x148;
constexpr std::size_t CalcSize = 0x10;
constexpr std::size_t BlockSize = 0x50;

inline std::uint16_t Rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t Rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// XOR every little-endian u16 with the high half of the advancing LCG.
inline void Crypt(std::uint8_t* data, std::size_t size, std::uint32_t seed) {
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        seed = 0x41C64E6Du * seed + 0x6073u;
        const std::uint16_t v = static_cast<std::uint16_t>(Rd16(data + i) ^ (seed >> 16));
        data[i] = static_cast<std::uint8_t>(v);
        data[i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
}

// PKHeX BlockPosition order ABCD..DCBA; shuffle values 24..31 repeat 0..7.
inline constexpr std::array<const char*, 24> Orders{
    "ABCD", "ABDC", "ACBD", "ACDB", "ADBC", "ADCB", "BACD", "BADC", "BCAD", "BCDA", "BDAC", "BDCA",
    "CABD", "CADB", "CBAD", "CBDA", "CDAB", "CDBA", "DABC", "DACB", "DBAC", "DBCA", "DCAB", "DCBA"};

struct Mon {
    std::uint32_t ec = 0;
    std::uint16_t species = 0;
    std::uint16_t held_item = 0;
    std::uint16_t tid = 0, sid = 0;
    std::uint32_t pid = 0;
    std::uint8_t nature = 0;
    std::uint8_t gender = 0; // 0 male, 1 female, 2 genderless
    std::uint8_t form = 0;
    std::uint16_t ability = 0;
    bool is_egg = false;
    bool shiny = false;
    std::u16string nickname;
    std::array<std::uint16_t, 4> moves{};
    std::array<std::uint8_t, 4> pp{};
    std::array<std::uint8_t, 4> pp_ups{};
    std::uint16_t hp = 0;
    std::uint32_t status = 0;
    std::uint8_t friendship = 0; // current handler's friendship (OT 0x112, HT 0xC8 when 0xC4 != 0)
    // calc block
    std::uint16_t level = 0, hp_max = 0, atk = 0, def = 0, spe = 0, spa = 0, spd = 0;
};

// PB8 persists Pml.PokePara.Sick directly at0x94, not the older status bitmask.
// Native Accessor.GetSick reads blockB+0x3C; CoreParam.GetSick returns it unchanged.
inline int Sick(std::uint32_t raw) { return raw <= 5 ? static_cast<int>(raw) : -1; }
inline bool MedicineCures(std::uint32_t raw, std::uint32_t flags) {
    constexpr int recovery_bit[] = {0, 19, 15, 18, 17, 16};
    const int sick = Sick(raw);
    return sick > 0 && (flags & (1u << recovery_bit[sick])) != 0;
}

// Stable ownership for UI actions; mutable battle/field values need their own preconditions.
inline bool SameIdentity(const Mon& expected, const Mon& current) {
    return expected.ec == current.ec && expected.pid == current.pid &&
           expected.tid == current.tid && expected.sid == current.sid &&
           expected.species == current.species && expected.form == current.form &&
           expected.is_egg == current.is_egg;
}

// Presentation must never expose an Egg's encrypted future Pokémon identity or stats.
// Keep the decoded gameplay copy intact for native editing and hatch detection.
inline Mon DisplayMon(const Mon& mon) {
    if (!mon.is_egg) return mon;
    Mon egg;
    egg.is_egg = true;
    egg.gender = 2;
    return egg;
}

// Decrypted, unshuffled core block; false on a checksum mismatch (the block is still filled, for a
// re-encode after an edit).
inline bool DecryptRaw(const std::uint8_t* core_enc, std::array<std::uint8_t, CoreSize>& plain) {
    std::array<std::uint8_t, CoreSize> c{};
    std::memcpy(c.data(), core_enc, CoreSize);
    const std::uint32_t ec = Rd32(c.data());
    Crypt(c.data() + 8, CoreSize - 8, ec);
    std::memcpy(plain.data(), c.data(), 8);
    const char* order = Orders[((ec >> 13) & 31) % 24];
    for (int i = 0; i < 4; ++i) {
        const int src = static_cast<int>(std::strchr(order, "ABCD"[i]) - order);
        std::memcpy(plain.data() + 8 + i * BlockSize, c.data() + 8 + src * BlockSize, BlockSize);
    }
    std::uint16_t sum = 0;
    for (std::size_t i = 8; i < CoreSize; i += 2)
        sum = static_cast<std::uint16_t>(sum + Rd16(plain.data() + i));
    return sum == Rd16(plain.data() + 6);
}

// Field view of a decrypted core block; `calc_enc` (encrypted, may be null) fills the calc stats.
inline Mon ParsePlain(const std::array<std::uint8_t, CoreSize>& d, const std::uint8_t* calc_enc) {
    Mon m;
    m.ec = Rd32(d.data());
    m.species = Rd16(d.data() + 0x08);
    m.held_item = Rd16(d.data() + 0x0A);
    m.tid = Rd16(d.data() + 0x0C);
    m.sid = Rd16(d.data() + 0x0E);
    m.ability = Rd16(d.data() + 0x14);
    m.pid = Rd32(d.data() + 0x1C);
    m.nature = d[0x20];
    m.gender = static_cast<std::uint8_t>((d[0x22] >> 2) & 3);
    m.form = d[0x24];
    m.is_egg = ((Rd32(d.data() + 0x8C) >> 30) & 1) != 0; // IV32 bit 30
    const std::uint32_t xr = static_cast<std::uint32_t>(m.tid ^ m.sid) ^ (m.pid >> 16) ^ (m.pid & 0xFFFF);
    m.shiny = xr < 16;
    for (int i = 0; i < 12; ++i) {
        const std::uint16_t ch = Rd16(d.data() + 0x58 + i * 2);
        if (ch == 0)
            break;
        m.nickname.push_back(static_cast<char16_t>(ch));
    }
    for (int i = 0; i < 4; ++i) {
        m.moves[i] = Rd16(d.data() + 0x72 + i * 2);
        m.pp[i] = d[0x7A + i];
        m.pp_ups[i] = d[0x7E + i];
    }
    m.hp = Rd16(d.data() + 0x8A);
    m.friendship = d[0xC4] == 0 ? d[0x112] : d[0xC8];
    m.status = Rd32(d.data() + 0x94);
    if (calc_enc) {
        std::array<std::uint8_t, CalcSize> k{};
        std::memcpy(k.data(), calc_enc, CalcSize);
        Crypt(k.data(), CalcSize, m.ec);
        m.level = Rd16(k.data() + 0);
        m.hp_max = Rd16(k.data() + 2);
        m.atk = Rd16(k.data() + 4);
        m.def = Rd16(k.data() + 6);
        m.spe = Rd16(k.data() + 8);
        m.spa = Rd16(k.data() + 10);
        m.spd = Rd16(k.data() + 12);
    }
    return m;
}

// Decrypts copies of the two arrays. Reject native invalid/decrypted flags and checksum
// mismatches so torn reads and invalid Pokémon cannot be published or edited. `plain_out`
// receives the decrypted core of an accepted Pokémon (for an edit and re-encode).
inline std::optional<Mon> Decode(const std::uint8_t* core_enc, const std::uint8_t* calc_enc,
                                 std::array<std::uint8_t, CoreSize>* plain_out = nullptr) {
    if (Rd16(core_enc + 4) != 0)
        return std::nullopt;
    if (Rd32(core_enc) == 0 && Rd16(core_enc + 6) == 0)
        return std::nullopt;
    std::array<std::uint8_t, CoreSize> d{};
    if (!DecryptRaw(core_enc, d))
        return std::nullopt;
    if (plain_out)
        *plain_out = d;
    return ParsePlain(d, calc_enc);
}

// Inverse of DecryptRaw; recomputes the checksum.
inline void EncryptRaw(std::array<std::uint8_t, CoreSize> plain, std::uint8_t* core_enc) {
    std::uint16_t sum = 0;
    for (std::size_t i = 8; i < CoreSize; i += 2)
        sum = static_cast<std::uint16_t>(sum + Rd16(plain.data() + i));
    plain[6] = static_cast<std::uint8_t>(sum);
    plain[7] = static_cast<std::uint8_t>(sum >> 8);
    const std::uint32_t ec = Rd32(plain.data());
    std::memcpy(core_enc, plain.data(), 8);
    const char* order = Orders[((ec >> 13) & 31) % 24];
    for (int i = 0; i < 4; ++i) {
        const int src = static_cast<int>(std::strchr(order, "ABCD"[i]) - order);
        std::memcpy(core_enc + 8 + src * BlockSize, plain.data() + 8 + i * BlockSize, BlockSize);
    }
    Crypt(core_enc + 8, CoreSize - 8, ec);
}

// Max PP with PP Ups (Gen 8 rule): base + base/5 * ups.
inline int MaxPp(int base_pp, int ups) {
    return base_pp + (base_pp / 5) * ups;
}

inline std::string Utf8(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) {
        const std::uint32_t u = c;
        if (u < 0x80) {
            o.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (u >> 6)));
            o.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xE0 | (u >> 12)));
            o.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return o;
}

} // namespace lp_pk8
