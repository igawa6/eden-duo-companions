// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// PK8 (Gen 8 party/box Pokémon, 0x158/0x148 bytes) decrypt, ported line-for-line from a
// Python script's crypt_array_u16/unshuffle_pk8_blocks/decrypt_pk8 written for this cycle,
// itself a 1:1 port of PKHeX.Core's PokeCrypto.cs (kwsch/PKHeX, MIT). Verified against a real
// party slot decrypted from a live Sword 1.3.2 save -- see 0100ABF008968000_test.cpp.
//
// This header only decrypts/decodes bytes already in hand; it does no I/O. The live-RAM address
// of an encrypted PK8 (party or box) is a separate, currently unverified problem -- see
// sw_addresses.h ("party.live"/"party.box_start") -- so nothing in sw_reader.h calls
// DecodeParty() with real guest bytes yet.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sw_pk8 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr std::size_t kBlockSize = 0x50;   // PokeCrypto.cs: SIZE_8BLOCK
constexpr std::size_t kSizeStored = 0x148; // SIZE_8STORED (box format)
constexpr std::size_t kSizeParty = 0x158;  // SIZE_8PARTY (adds the 0x10-byte party stat tail)
constexpr std::size_t kBlockCount = 4;

// PokeCrypto.cs: BlockPosition. Values 24-31 repeat 0-7 "to eliminate modulus" (the source
// comment, carried over verbatim): sv = (pid >> 13) & 31 indexes groups of 4 here directly,
// without a second `% 24`.
inline constexpr u8 kBlockPosition[128]{
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
    2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2,
    2, 3, 0, 1, 3, 2, 0, 1, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0,
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
};

inline u16 Rd16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
inline u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline void Wr16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

// PokeCrypto.CryptArray: an LCG keystream XORed over little-endian u16 words. `seed` starts at
// the Pokémon's EncryptionConstant; an odd-length tail's last byte is left untouched (matches
// script's `n = len(data) - (len(data) % 2)`, which never occurs for the fixed PK8 sizes
// this header uses but is kept for a general buffer).
inline void CryptArrayU16(u8* data, std::size_t size, u32 seed) {
    const std::size_t n = size - (size % 2);
    for (std::size_t i = 0; i < n; i += 2) {
        seed = 0x41C64E6Du * seed + 0x00006073u;
        const u16 xor_word = static_cast<u16>(seed >> 16);
        Wr16(data + i, static_cast<u16>(Rd16(data + i) ^ xor_word));
    }
}

// PokeCrypto.Shuffle<ulong>(shuffle, sv, SIZE_8BLOCK), decrypt direction: a swap-based
// permutation of the four 0x50-byte sub-blocks, ported exactly (including the no-op early
// return for sv == 0) from the reference script's unshuffle_pk8_blocks.
inline void UnshuffleBlocks(u8* shuffle /* kBlockCount * kBlockSize bytes */, u32 sv) {
    if (sv == 0) return;
    const u8* order = &kBlockPosition[static_cast<std::size_t>(sv) * 4];
    int perm[kBlockCount]{0, 1, 2, 3};
    int slot_of[kBlockCount]{0, 1, 2, 3};
    for (int i = 0; i < static_cast<int>(kBlockCount) - 1; ++i) {
        const int desired = order[i];
        const int j = slot_of[desired];
        if (j == i) continue;
        u8 tmp[kBlockSize];
        std::memcpy(tmp, shuffle + static_cast<std::size_t>(i) * kBlockSize, kBlockSize);
        std::memcpy(shuffle + static_cast<std::size_t>(i) * kBlockSize,
                    shuffle + static_cast<std::size_t>(j) * kBlockSize, kBlockSize);
        std::memcpy(shuffle + static_cast<std::size_t>(j) * kBlockSize, tmp, kBlockSize);
        const int block_at_i = perm[i];
        perm[j] = block_at_i;
        slot_of[block_at_i] = j;
    }
}

// PokeCrypto.Decrypt8, in place. `member` must be `size` bytes, `size` one of kSizeStored
// (0x148, box) or kSizeParty (0x158, party; decrypts the 0x10-byte stat tail too). Not
// validated here (no checksum/species check) -- callers that read live/guest memory must do
// their own torn-read and sanity checks before trusting the result (MODULE_GUIDE.md section 6:
// "Reject torn reads", "No heuristics").
inline void DecryptInPlace(u8* member, std::size_t size) {
    const u32 pv = Rd32(member);
    const u32 sv = (pv >> 13) & 31;

    u8 shuffle[kBlockCount * kBlockSize];
    std::memcpy(shuffle, member + 8, sizeof shuffle);
    CryptArrayU16(shuffle, sizeof shuffle, pv);

    if (size > kSizeStored)
        CryptArrayU16(member + kSizeStored, size - kSizeStored, pv);

    UnshuffleBlocks(shuffle, sv);
    std::memcpy(member + 8, shuffle, sizeof shuffle);
}

// Highest species id SWSH itself can produce (Calyrex, the last species introduced across
// Sword/Shield + Isle of Armor + Crown Tundra). A sanity bound for live-read validation, not a
// claim about the National Dex in general -- see sw_reader.h's "never stale data" rule.
constexpr u16 kMaxKnownSpeciesSwSh = 898;

// PokeCrypto.GetCHK (PKHeX): the stored checksum at +0x06 is the 16-bit wrapping sum of every
// little-endian u16 word from +0x08 up to (not including) `size` (kSizeStored, i.e. it does not
// cover the 0x10-byte live party-stat tail). Bytes 0x00-0x07 (EncryptionConstant + Sanity +
// Checksum itself) are never part of the encrypted block region (DecryptInPlace only touches
// member+8 onward), so this reads identically on an encrypted or decrypted buffer -- callers
// here always pass the decrypted one.
inline u16 ComputeChecksum16(const u8* decrypted, std::size_t size = kSizeStored) {
    u32 sum = 0;
    for (std::size_t i = 8; i + 1 < size; i += 2) sum += Rd16(decrypted + i);
    return static_cast<u16>(sum & 0xFFFFu);
}

// A read-only view of the fields sw_reader.h's party domain publishes. Filled by DecodeParty()
// below; every field reads straight from the now-decrypted, unshuffled buffer at the same
// offsets the reference script's `info` command uses, plus the live-verified party-stat-tail
// fields (max HP, moves, PP) a future reader needs -- see LIVE-READERS-132.md's 2026-10-24
// session ("Readers, verified on 2 boots"). Current HP IS present in this buffer -- u16 at
// +0x8A, status condition is i32 at +0x94 (PKHeX G8PKM.cs) -- but it is NOT a live field here
// on purpose: LIVE-READERS-132.md's 2026-10-25 "gate" session proved the CaptureSight Party heap
// mirror (host of this whole buffer) is a save-file-time snapshot, not a per-frame live struct --
// +0x8A read a correct, on-screen-matching 240 right after a fresh save-import (and 0x94 read 0,
// matching "no status"), stayed frozen at 240 after an in-game Potion visibly moved the on-screen
// HP to 260 (same boot, no reconnect), and only became 260 itself after an explicit in-game Save.
// (An earlier same-day session's "240 not found anywhere, 0x00-0x157 exhaustively searched" note
// was very likely taken against a post-heal-but-not-yet-saved live state -- at that moment the
// heap mirror legitimately holds neither the old nor the new HP as "240"; it is consistent with
// this session's finding, not a contradiction of the offset.) A caller must not read +0x8A/+0x94
// from the live heap mirror and show it as current HP/status: it is truthful only immediately
// after an import or an explicit save, and silently stale every moment of actual play in between
// -- exactly the "never show stale values" failure mode this project's own rules forbid. The
// fields are decoded here (see max_hp/current_hp_at_save/status_at_save below) only for save-file
// tooling (e.g. a future box/trade viewer that reads main itself, not live memory); sw_reader.h
// must keep sourcing current HP/status from the live battle-mon struct
// (heap+0x8FE9D570/sw_battle.h) for the two active combatants and publish nothing for the rest of
// the party until a real live-updating source is found (still unresolved as of this session,
// despite dedicated RE work -- see LIVE-READERS-132.md's "battle-root" session).
// Nickname (PKHeX G8PKM.Nickname): 12 UTF-16LE code units + a 0x0000 terminator at decrypted
// +0x58..+0x71 (just before Move1 at +0x72). An un-nicknamed Pokemon stores its species name
// here, so this is always the name the native summary shows. Game-private glyphs E08E/E08F are
// the male/female signs (PKHeX StringConverter8 remap). Returns "" for an empty/broken field.
constexpr std::size_t kNicknameOffset = 0x58;
constexpr std::size_t kNicknameChars = 12;

inline std::string DecodeNicknameUtf8(const u8* decrypted) {
    std::string out;
    for (std::size_t i = 0; i < kNicknameChars + 1; ++i) {
        u32 c = static_cast<u32>(decrypted[kNicknameOffset + i * 2] | (decrypted[kNicknameOffset + i * 2 + 1] << 8));
        if (c == 0) return out;
        if (i == kNicknameChars) return {}; // no terminator: not a nickname field
        if (c == 0xE08E) c = 0x2642;
        else if (c == 0xE08F) c = 0x2640;
        if (c >= 0xD800 && c <= 0xDFFF) {
            if (c > 0xDBFF || i + 1 >= kNicknameChars) return {};
            const u32 lo = static_cast<u32>(decrypted[kNicknameOffset + (i + 1) * 2] |
                                            (decrypted[kNicknameOffset + (i + 1) * 2 + 1] << 8));
            if (lo < 0xDC00 || lo > 0xDFFF) return {};
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
        }
        if (c < 0x20) return {};
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

struct PartyMon {
    u16 species = 0;
    u8 level = 0;
    u32 encryption_constant = 0;
    u32 pid = 0;
    u16 tid = 0;         // +0x0C, original trainer's 16-bit TID
    u16 sid = 0;         // +0x0E, original trainer's 16-bit SID
    u32 experience = 0;
    u16 checksum = 0;
    // PokeCrypto.GetCHK recomputed over the decrypted buffer, compared against `checksum`.
    // A verified live reader must not trust a slot whose checksum does not match (torn read,
    // or a decode-offset mistake) -- see sw_reader.h.
    bool checksum_valid = false;
    // `species` is a plausible SWSH dex entry (1..kMaxKnownSpeciesSwSh). Separate from
    // `species != 0` ("not an empty slot") so a caller can tell "empty" from "garbage".
    bool species_in_range = false;
    u16 max_hp = 0;     // +0x14A, party-stat tail; VERIFIED against native 06-party.png (exact x/max match)
    u16 moves[4]{};     // +0x72/+0x74/+0x76/+0x78; VERIFIED live, sane move-ID range (<=900)
    u8 pp[4]{};         // +0x7A/+0x7B/+0x7C/+0x7D; VERIFIED live, sane PP range (<=40)
    // SAVE-FILE-TIME SNAPSHOT ONLY -- see the big comment above this struct. +0x8A/+0x94
    // (PKHeX G8PKM.cs), live-verified 2026-10-25: a fresh gb-635571 import read
    // current_hp_at_save=240 (Mewtwo, max_hp=356, matching the native party screen exactly) and
    // status_at_save=0; an in-game Potion moved the on-screen HP to 260 while this field stayed
    // frozen at 240 in the same boot; it only became 260 after an explicit in-game Save. Never
    // bind these to a live display -- they are accurate only right after an import/load/save and
    // stale for the rest of actual play. Kept for save-file-only tooling, not sw_reader.h.
    u16 current_hp_at_save = 0;  // +0x8A
    u32 status_at_save = 0;      // +0x94 (int32 in PKHeX; 0 == no status)
    bool egg = false;            // +0x8C bit30 (IsEgg); never reveal species/stats for an Egg
    u8 form = 0;                 // +0x24 (types/matchups are per form)
    std::string nickname;        // +0x58, UTF-8 ("" = undecodable)
};

// Decrypts a copy of `encrypted` (kSizeParty bytes) and reads the handful of fields a party-list
// page needs. Species 0 means an empty slot. No side effects on `encrypted`.
inline PartyMon DecodeParty(const u8* encrypted) {
    std::array<u8, kSizeParty> buf{};
    std::memcpy(buf.data(), encrypted, kSizeParty);
    DecryptInPlace(buf.data(), kSizeParty);

    PartyMon m;
    m.encryption_constant = Rd32(buf.data() + 0x00);
    m.checksum = Rd16(buf.data() + 0x06);
    m.species = Rd16(buf.data() + 0x08);
    m.pid = Rd32(buf.data() + 0x1C);
    m.tid = Rd16(buf.data() + 0x0C);
    m.sid = Rd16(buf.data() + 0x0E);
    m.experience = Rd32(buf.data() + 0x10);
    m.level = buf[0x148];
    m.max_hp = Rd16(buf.data() + 0x14A);
    m.moves[0] = Rd16(buf.data() + 0x72);
    m.moves[1] = Rd16(buf.data() + 0x74);
    m.moves[2] = Rd16(buf.data() + 0x76);
    m.moves[3] = Rd16(buf.data() + 0x78);
    m.pp[0] = buf[0x7A];
    m.pp[1] = buf[0x7B];
    m.pp[2] = buf[0x7C];
    m.pp[3] = buf[0x7D];
    m.current_hp_at_save = Rd16(buf.data() + 0x8A);
    m.status_at_save = Rd32(buf.data() + 0x94);
    m.egg = ((Rd32(buf.data() + 0x8C) >> 30) & 1u) != 0;
    m.form = buf[0x24];
    m.nickname = DecodeNicknameUtf8(buf.data());
    m.checksum_valid = ComputeChecksum16(buf.data()) == m.checksum;
    m.species_in_range = m.species >= 1 && m.species <= kMaxKnownSpeciesSwSh;
    return m;
}

// Live party-slot object (sw_addresses.h's "party.array_root" anchor): the 0x148-byte encrypted
// "stored" PK8 at slot_ptr+0xD0 -- see LIVE-READERS-132.md's "Live party objects FOUND" session.
// Decrypts with the same DecryptInPlace routine, just with size==kSizeStored (the tail branch is
// a no-op, matching the Python reference tool's own `decrypt_pk8` on a kSizeStored-length
// buffer). Unlike the SAVE-snapshot mirror above, current HP (+0x8A) and status (+0x94) here ARE
// live -- verified 2026-10-25, 2 independent boots, 2 Potion uses, no save needed (see the file
// header session). MaxHP/Level are NOT present in this smaller buffer (no party-stat tail) -- a
// caller computes them from `experience`/`iv_hp`/`ev_hp` plus the species' own personal-table
// base HP/growth rate (sw_personal_stats.h + sw_level.h), then should cross-check the result
// against the save mirror's own MaxHP/Level before trusting it (0100ABF008968000_test.cpp's
// TestComputeMaxHpMatchesSaveMirror does exactly this for all 5 gb-635571 fixture members).
struct LivePartyMon {
    u16 species = 0;
    u16 checksum = 0;
    bool checksum_valid = false;
    bool species_in_range = false;
    u32 experience = 0; // +0x10
    u8 iv_hp = 0;        // +0x8C, bits 0-4 (IV32 & 0x1F)
    u8 ev_hp = 0;         // +0x26
    u16 current_hp = 0;  // +0x8A, LIVE (not a save-time snapshot -- see the struct's own header)
    u32 status = 0;      // +0x94, LIVE (int32 in PKHeX; 0 == no status)
    u16 moves[4]{};      // +0x72, same stored-block offsets as PartyMon above
    u8 pp[4]{};          // +0x7A, live PP
    u8 pp_ups[4]{};      // +0x7E, PKHeX G8PKM.Move1..4_PPUps (0..3)
    // Read-only summary fields: PKHeX.Core/PKM/Shared/G8PKM.cs (MIT), checked 2026-10-28.
    u16 held_item = 0;   // +0x0A
    u16 tid = 0;         // +0x0C, original trainer's 16-bit TID
    u16 sid = 0;         // +0x0E, original trainer's 16-bit SID
    u32 pid = 0;         // +0x1C
    u16 ability = 0;     // +0x14
    u8 nature = 0;       // +0x20, original nature
    u8 stat_nature = 0;  // +0x21, effective (mint-adjusted) stat alignment
    u8 gender = 2;       // +0x22 bits2..3, 0 male/1 female/2 genderless
    u8 form = 0;         // +0x24
    u8 hyper_train = 0;  // +0x126; bit order HP/Atk/Def/SpA/SpD/Spe
    bool egg = false;    // +0x8C bit30; summaries must not reveal an Egg's contents
    std::string nickname; // +0x58, UTF-8 ("" = undecodable); species name when not nicknamed
    std::array<u8, 6> iv{}; // native stat order HP/Atk/Def/Spe/SpA/SpD
    std::array<u8, 6> ev{};
};

/// Shiny (PKHeX PKM.IsShiny, Gen 8): the PID's two halves XOR the original trainer's TID/SID
/// below 16. Gen 8 also stores the shiny type: 0 = a "square" shiny, 1..15 = a "star" shiny
/// (only the entrance sparkle differs; the menus show one mark for both).
inline u32 ShinyXor(u32 pid, u16 tid, u16 sid) { return (pid >> 16) ^ (pid & 0xFFFFu) ^ tid ^ sid; }
inline bool IsShiny(u32 pid, u16 tid, u16 sid) { return ShinyXor(pid, tid, sid) < 16; }

/// Decrypts a copy of `encrypted` (kSizeStored == 0x148 bytes, the live slot object's blob) and
/// reads the fields a live party reader needs. No side effects on `encrypted`.
inline LivePartyMon DecodeLiveStored(const u8* encrypted) {
    std::array<u8, kSizeStored> buf{};
    std::memcpy(buf.data(), encrypted, kSizeStored);
    DecryptInPlace(buf.data(), kSizeStored);

    LivePartyMon m;
    m.checksum = Rd16(buf.data() + 0x06);
    m.species = Rd16(buf.data() + 0x08);
    m.experience = Rd32(buf.data() + 0x10);
    const u32 iv32 = Rd32(buf.data() + 0x8C);
    m.iv_hp = static_cast<u8>(iv32 & 0x1Fu);
    m.ev_hp = buf[0x26];
    m.held_item = Rd16(buf.data() + 0x0A);
    m.tid = Rd16(buf.data() + 0x0C);
    m.sid = Rd16(buf.data() + 0x0E);
    m.pid = Rd32(buf.data() + 0x1C);
    m.ability = Rd16(buf.data() + 0x14);
    m.nature = buf[0x20];
    m.stat_nature = buf[0x21];
    m.gender = static_cast<u8>((buf[0x22] >> 2) & 3u);
    m.form = buf[0x24];
    m.hyper_train = buf[0x126];
    m.egg = (iv32 & (1u << 30)) != 0;
    m.nickname = DecodeNicknameUtf8(buf.data());
    for (int i = 0; i < 6; ++i) {
        m.iv[static_cast<std::size_t>(i)] = static_cast<u8>((iv32 >> (i * 5)) & 31u);
        m.ev[static_cast<std::size_t>(i)] = buf[0x26 + i];
    }
    m.current_hp = Rd16(buf.data() + 0x8A);
    m.status = Rd32(buf.data() + 0x94);
    for (int i = 0; i < 4; ++i) {
        m.moves[i] = Rd16(buf.data() + 0x72 + i * 2);
        m.pp[i] = buf[0x7A + i];
        m.pp_ups[i] = buf[0x7E + i];
    }
    m.checksum_valid = ComputeChecksum16(buf.data(), kSizeStored) == m.checksum;
    m.species_in_range = m.species >= 1 && m.species <= kMaxKnownSpeciesSwSh;
    return m;
}

} // namespace sw_pk8
