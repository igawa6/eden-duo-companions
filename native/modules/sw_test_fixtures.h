// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tests only, never part of a module.
//
// 1. Private fixtures. No game file and no save data is in this repository. The checks that
//    compare against real bytes read them at run time: the game's own files from the player's
//    extracted romfs (SW_ROMFS_DIR, see sw_test_romfs.h) and blocks of a private save from
//    SW_FIXTURES_DIR (binary files such as k_item.bin, see the test that loads each). A test
//    binary run with --private-data does only those checks and exits with kSkip (CTest
//    SKIP_RETURN_CODE) when the directories are not set.
// 2. Synthetic PK8s. SynMon describes a made-up Pokemon by its field values; MakeStored /
//    MakeParty write and encrypt it with the inverse of sw_pk8::DecryptInPlace, so the reader
//    tests run on structure-faithful blobs that come from no save.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sw_pk8.h"
#include "sw_test_romfs.h"

namespace sw_test {

inline constexpr int kSkip = 77;

inline const char* FixturesDir() {
    const char* d = std::getenv("SW_FIXTURES_DIR");
    return d && *d ? d : nullptr;
}

inline std::vector<unsigned char> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/// A file of SW_FIXTURES_DIR. Aborts when the directory is set but the file is missing or not
/// `expect_size` bytes (a broken private setup must fail, not pass quietly).
inline std::vector<unsigned char> Fixture(const char* name, std::size_t expect_size) {
    auto bytes = ReadFileBytes(std::string(FixturesDir()) + "/" + name);
    if (bytes.size() != expect_size) {
        std::fprintf(stderr, "SW_FIXTURES_DIR/%s: expected %zu bytes, got %zu\n", name, expect_size, bytes.size());
        std::abort();
    }
    return bytes;
}

/// A file of the player's romfs (path relative to SW_ROMFS_DIR, e.g. "/bin/pml/waza/waza0001.wazabin").
inline std::vector<unsigned char> RomfsFile(const std::string& rel) {
    auto bytes = ReadFileBytes(std::string(RomfsDir()) + rel);
    if (bytes.empty()) {
        std::fprintf(stderr, "SW_ROMFS_DIR%s: missing\n", rel.c_str());
        std::abort();
    }
    return bytes;
}

/// True when both private inputs are set; prints why not otherwise.
inline bool PrivateDataAvailable() {
    if (FixturesDir() && RomfsDir()) return true;
    std::printf("skipped: real-data checks need SW_FIXTURES_DIR (private save fixtures) and "
                "SW_ROMFS_DIR (the player's extracted romfs)\n");
    return false;
}

// ------------------------------------------------------------------------------ synthetic PK8

struct SynMon {
    std::uint32_t ec = 0x5EED0001u;   // EncryptionConstant: also selects the block shuffle
    std::uint32_t pid = 0x2B1C0D3Eu;  // not shiny for TID/SID 0/0
    std::uint16_t species = 0;
    std::uint8_t form = 0;
    std::uint16_t held_item = 0;
    std::uint16_t tid = 0, sid = 0;
    std::uint32_t experience = 0;
    std::uint16_t ability = 0;
    std::uint8_t nature = 0, stat_nature = 0;
    std::uint8_t gender = 2;          // genderless
    std::array<std::uint8_t, 6> ev{}; // native order HP/Atk/Def/Spe/SpA/SpD
    std::array<std::uint8_t, 6> iv{};
    bool egg = false;
    std::u16string nickname;          // UTF-16, at most 12 code units
    std::array<std::uint16_t, 4> moves{};
    std::array<std::uint8_t, 4> pp{};
    std::array<std::uint8_t, 4> pp_ups{};
    std::uint16_t current_hp = 0;
    std::uint32_t status = 0;
    // Party-stat tail (MakeParty only).
    std::uint8_t level = 0;
    std::array<std::uint16_t, 6> stats{}; // MaxHP, Atk, Def, Spe, SpA, SpD at +0x14A
};

inline void Put16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
}
inline void Put32(unsigned char* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

/// The decrypted 0x158 layout (PKHeX G8PKM offsets, the same ones sw_pk8.h decodes), checksum set.
inline std::array<unsigned char, sw_pk8::kSizeParty> PlainParty(const SynMon& m) {
    std::array<unsigned char, sw_pk8::kSizeParty> d{};
    unsigned char* b = d.data();
    Put32(b + 0x00, m.ec);
    Put16(b + 0x08, m.species);
    Put16(b + 0x0A, m.held_item);
    Put16(b + 0x0C, m.tid);
    Put16(b + 0x0E, m.sid);
    Put32(b + 0x10, m.experience);
    Put16(b + 0x14, m.ability);
    Put32(b + 0x1C, m.pid);
    b[0x20] = m.nature;
    b[0x21] = m.stat_nature;
    b[0x22] = static_cast<unsigned char>((m.gender & 3u) << 2);
    b[0x24] = m.form;
    for (int i = 0; i < 6; ++i) b[0x26 + i] = m.ev[static_cast<std::size_t>(i)];
    for (std::size_t i = 0; i < m.nickname.size() && i < sw_pk8::kNicknameChars; ++i)
        Put16(b + sw_pk8::kNicknameOffset + i * 2, static_cast<std::uint16_t>(m.nickname[i]));
    for (int i = 0; i < 4; ++i) {
        Put16(b + 0x72 + i * 2, m.moves[static_cast<std::size_t>(i)]);
        b[0x7A + i] = m.pp[static_cast<std::size_t>(i)];
        b[0x7E + i] = m.pp_ups[static_cast<std::size_t>(i)];
    }
    Put16(b + 0x8A, m.current_hp);
    std::uint32_t iv32 = 0;
    for (int i = 0; i < 6; ++i) iv32 |= static_cast<std::uint32_t>(m.iv[static_cast<std::size_t>(i)] & 31u) << (5 * i);
    if (m.egg) iv32 |= 1u << 30;
    Put32(b + 0x8C, iv32);
    Put32(b + 0x94, m.status);
    b[0x148] = m.level;
    for (int i = 0; i < 6; ++i) Put16(b + 0x14A + 2 * i, m.stats[static_cast<std::size_t>(i)]);
    Put16(b + 0x06, sw_pk8::ComputeChecksum16(b, sw_pk8::kSizeStored));
    return d;
}

/// PokeCrypto.Encrypt8, the inverse of sw_pk8::DecryptInPlace: shuffle the four sub-blocks back
/// (the permutation read off UnshuffleBlocks itself), XOR the keystream, and the tail's own
/// keystream for a party-size buffer. The checksum is left as it is.
inline void EncryptInPlace(unsigned char* member, std::size_t size) {
    const std::uint32_t pv = sw_pk8::Rd32(member);
    const std::uint32_t sv = (pv >> 13) & 31;
    constexpr std::size_t kBlock = sw_pk8::kBlockSize, kBlocks = sw_pk8::kBlockCount;
    unsigned char tags[kBlocks * kBlock];
    for (std::size_t b = 0; b < kBlocks; ++b) std::memset(tags + b * kBlock, static_cast<int>(b), kBlock);
    sw_pk8::UnshuffleBlocks(tags, sv); // decrypted block i came from shuffled block tags[i * kBlock]
    unsigned char shuffled[kBlocks * kBlock];
    for (std::size_t i = 0; i < kBlocks; ++i)
        std::memcpy(shuffled + tags[i * kBlock] * kBlock, member + 8 + i * kBlock, kBlock);
    sw_pk8::CryptArrayU16(shuffled, sizeof shuffled, pv);
    std::memcpy(member + 8, shuffled, sizeof shuffled);
    if (size > sw_pk8::kSizeStored) sw_pk8::CryptArrayU16(member + sw_pk8::kSizeStored, size - sw_pk8::kSizeStored, pv);
}

/// An encrypted 0x148 "stored" blob (the live party slot object's format).
inline std::array<unsigned char, sw_pk8::kSizeStored> MakeStored(const SynMon& m) {
    const auto plain = PlainParty(m);
    std::array<unsigned char, sw_pk8::kSizeStored> out{};
    std::memcpy(out.data(), plain.data(), out.size());
    EncryptInPlace(out.data(), out.size());
    return out;
}

/// An encrypted 0x158 party blob (the save mirror's format, with the party-stat tail).
inline std::array<unsigned char, sw_pk8::kSizeParty> MakeParty(const SynMon& m) {
    auto out = PlainParty(m);
    EncryptInPlace(out.data(), out.size());
    return out;
}

// The made-up party the reader tests share. Species, level-100 Slow-growth EXP and the HP-side
// values are what the tests compute against; every other field (EC/PID, nickname, moves, items,
// abilities) is invented. MaxHP at level 100 with base HP b: 2b + 31 + 14/4 + 110.
inline SynMon SynPartyMember(int which) {
    SynMon m;
    m.experience = 1250000u; // Slow growth, level 100
    m.iv.fill(31);
    m.ev = {14, 0, 0, 0, 0, 0};
    m.pp = {10, 10, 10, 10};
    m.nickname = u"测试"; // two CJK code points the package font has no glyphs for
    switch (which) {
    case 0: // base HP 106 -> MaxHP 356
        m.ec = 0x5EED2A01u; m.pid = 0x11112222u; m.species = 150; m.current_hp = 240;
        m.moves = {1, 2, 3, 4}; m.ability = 1;
        break;
    case 1: // base HP 140 -> 424
        m.ec = 0x5EED6B02u; m.pid = 0x33334444u; m.species = 890; m.current_hp = 424;
        m.moves = {5, 6, 7, 8}; m.ability = 2;
        break;
    case 2: // base HP 92 -> 328
        m.ec = 0x5EEDA403u; m.pid = 0x55556666u; m.species = 888; m.current_hp = 328;
        m.moves = {9, 10, 11, 12}; m.held_item = 1; m.ability = 3;
        break;
    case 3: // base HP 92 -> 328
        m.ec = 0x5EEDE504u; m.pid = 0x77778888u; m.species = 889; m.current_hp = 328;
        m.moves = {13, 14, 15, 16}; m.held_item = 2; m.ability = 4;
        break;
    default: // base HP 105, EV 0 -> 351
        m.ec = 0x5EED1C05u; m.pid = 0x9999AAAAu; m.species = 893; m.current_hp = 351;
        m.ev = {0, 0, 0, 0, 0, 0}; m.moves = {17, 18, 19, 20}; m.ability = 5;
        break;
    }
    return m;
}

} // namespace sw_test
