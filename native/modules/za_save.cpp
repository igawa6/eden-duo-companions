// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "za_save.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "za_sha256.h"

namespace za {
namespace {

// The container's fixed obfuscation pad: 0x7F bytes, repeated over the payload. (The reference
// implementation keeps a 0x80-byte copy whose last byte is zero and steps 0x7F at a time, which
// is the same thing as a 0x7F period.)
constexpr u8 kXorPad[0x7F] = {
    0xA0, 0x92, 0xD1, 0x06, 0x07, 0xDB, 0x32, 0xA1, 0xAE, 0x01, 0xF5, 0xC5, 0x1E, 0x84, 0x4F, 0xE3,
    0x53, 0xCA, 0x37, 0xF4, 0xA7, 0xB0, 0x4D, 0xA0, 0x18, 0xB7, 0xC2, 0x97, 0xDA, 0x5F, 0x53, 0x2B,
    0x75, 0xFA, 0x48, 0x16, 0xF8, 0xD4, 0x8A, 0x6F, 0x61, 0x05, 0xF4, 0xE2, 0xFD, 0x04, 0xB5, 0xA3,
    0x0F, 0xFC, 0x44, 0x92, 0xCB, 0x32, 0xE6, 0x1B, 0xB9, 0xB1, 0x2E, 0x01, 0xB0, 0x56, 0x53, 0x36,
    0xD2, 0xD1, 0x50, 0x3D, 0xDE, 0x5B, 0x2E, 0x0E, 0x52, 0xFD, 0xDF, 0x2F, 0x7B, 0xCA, 0x63, 0x50,
    0xA4, 0x67, 0x5D, 0x23, 0x17, 0xC0, 0x52, 0xE1, 0xA6, 0x30, 0x7C, 0x2B, 0xB6, 0x70, 0x36, 0x5B,
    0x2A, 0x27, 0x69, 0x33, 0xF5, 0x63, 0x7B, 0x36, 0x3F, 0x26, 0x9B, 0xA3, 0xED, 0x7A, 0x53, 0x00,
    0xA4, 0x48, 0xB3, 0x50, 0x9E, 0x14, 0xA0, 0x52, 0xDE, 0x7E, 0x10, 0x2B, 0x1B, 0x77, 0x6E,
};

constexpr u8 kIntro[0x40] = {
    0x9E, 0xC9, 0x9C, 0xD7, 0x0E, 0xD3, 0x3C, 0x44, 0xFB, 0x93, 0x03, 0xDC, 0xEB, 0x39, 0xB4, 0x2A,
    0x19, 0x47, 0xE9, 0x63, 0x4B, 0xA2, 0x33, 0x44, 0x16, 0xBF, 0x82, 0xA2, 0xBA, 0x63, 0x55, 0xB6,
    0x3D, 0x9D, 0xF2, 0x4B, 0x5F, 0x7B, 0x6A, 0xB2, 0x62, 0x1D, 0xC2, 0x1B, 0x68, 0xE5, 0xC8, 0xB5,
    0x3A, 0x05, 0x90, 0x00, 0xE8, 0xA8, 0x10, 0x3D, 0xE2, 0xEC, 0xF0, 0x0C, 0xB2, 0xED, 0x4F, 0x6D,
};

constexpr u8 kOutro[0x40] = {
    0xD6, 0xC0, 0x1C, 0x59, 0x8B, 0xC8, 0xB8, 0xCB, 0x46, 0xE1, 0x53, 0xFC, 0x82, 0x8C, 0x75, 0x75,
    0x13, 0xE0, 0x45, 0xDF, 0x32, 0x69, 0x3C, 0x75, 0xF0, 0x59, 0xF8, 0xD9, 0xA2, 0x5F, 0xB2, 0x17,
    0xE0, 0x80, 0x52, 0xDB, 0xEA, 0x89, 0x73, 0x99, 0x75, 0x79, 0xAF, 0xCB, 0x2E, 0x80, 0x07, 0xE6,
    0xF1, 0x26, 0xE0, 0x03, 0x0A, 0xE6, 0x6F, 0xF6, 0x41, 0xBF, 0x7E, 0x59, 0xC2, 0xAE, 0x55, 0xFD,
};

constexpr std::size_t kHashSize = 0x20;

// The block shuffle table: 24 four-block permutations, then duplicates of the first eight so a
// five-bit selector needs no modulus (32 groups of four = 128 entries).
constexpr u8 kBlockPosition[128] = {
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2,
    0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
    2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1,
    1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2,
    2, 3, 0, 1, 3, 2, 0, 1, 1, 2, 3, 0, 1, 3, 2, 0,
    2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0,
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2,
    0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
};

u16 Le16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Le32(const u8* p) {
    return static_cast<u32>(Le16(p)) | (static_cast<u32>(Le16(p + 2)) << 16);
}
u64 Le64(const u8* p) {
    return static_cast<u64>(Le32(p)) | (static_cast<u64>(Le32(p + 4)) << 32);
}
float LeF32(const u8* p) {
    const u32 bits = Le32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
double LeF64(const u8* p) {
    const u64 bits = Le64(p);
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

// ---- SwishCrypto block cipher ----------------------------------------------------------------

class XorShift32 {
public:
    explicit XorShift32(u32 seed) : state_(Init(seed)) {}

    u8 Next() {
        const u8 value = static_cast<u8>(state_ >> (counter_ << 3));
        if (counter_ == 3) {
            state_ = Advance(state_);
            counter_ = 0;
        } else {
            ++counter_;
        }
        return value;
    }

    u32 Next32() {
        return static_cast<u32>(Next()) | (static_cast<u32>(Next()) << 8) |
               (static_cast<u32>(Next()) << 16) | (static_cast<u32>(Next()) << 24);
    }

private:
    static u32 Advance(u32 s) {
        s ^= s << 2;
        s ^= s >> 15;
        s ^= s << 13;
        return s;
    }
    static u32 Init(u32 state) {
        int pop = 0;
        for (u32 v = state; v; v &= v - 1)
            ++pop;
        for (int i = 0; i < pop; ++i)
            state = Advance(state);
        return state;
    }

    u32 state_;
    int counter_ = 0;
};

// Element size of a scalar block type, or -1 for a type this walk does not understand.
int TypeSize(u8 type) {
    switch (type) {
    case 1: // Bool1
    case 2: // Bool2
    case 3: // Bool3
        return 0; // zero-length blocks
    case 8: // Byte
    case 12: // SByte
        return 1;
    case 9: // UInt16
    case 13: // Int16
        return 2;
    case 10: // UInt32
    case 14: // Int32
    case 16: // Single
        return 4;
    case 11: // UInt64
    case 15: // Int64
    case 17: // Double
        return 8;
    default:
        return -1; // Object/Array are handled by the caller; anything else ends the walk
    }
}

} // namespace

bool ReadBlocks(std::vector<u8>& payload, std::vector<BlockView>& out) {
    out.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (payload.size() - offset < 5)
            return false;
        const u32 key = Le32(payload.data() + offset);
        offset += 4;
        XorShift32 xk(key);
        const u8 type = static_cast<u8>(payload[offset++] ^ xk.Next());
        std::size_t size = 0;
        if (type == 4) { // Object: 4-byte length, then the bytes
            if (payload.size() - offset < 4)
                return false;
            const u32 length = Le32(payload.data() + offset) ^ xk.Next32();
            offset += 4;
            size = length;
        } else if (type == 5) { // Array: 4-byte count, 1-byte element type
            if (payload.size() - offset < 5)
                return false;
            const u32 count = Le32(payload.data() + offset) ^ xk.Next32();
            offset += 4;
            const u8 sub = static_cast<u8>(payload[offset++] ^ xk.Next());
            const int element = TypeSize(sub);
            if (element < 0)
                return false;
            size = static_cast<std::size_t>(count) * static_cast<std::size_t>(element);
        } else {
            const int element = TypeSize(type);
            if (element < 0)
                return false;
            size = static_cast<std::size_t>(element);
        }
        if (size > payload.size() - offset)
            return false;
        // The block's own bytes are obfuscated with the same stream, continuing past the header.
        for (std::size_t i = 0; i < size; ++i)
            payload[offset + i] ^= xk.Next();
        out.push_back(BlockView{key, payload.data() + offset, size});
        offset += size;
    }
    return !out.empty();
}

const BlockView* FindBlock(const std::vector<BlockView>& blocks, u32 key) {
    for (const auto& block : blocks) {
        if (block.key == key)
            return &block;
    }
    return nullptr;
}

bool DecryptEntity(const u8* raw, std::size_t size, Entity& out) {
    out = Entity{};
    if (!raw || size < kStoredSize)
        return false;
    const std::size_t usable = std::min<std::size_t>(size, kPartySize);
    u8 work[kPartySize];
    std::memcpy(work, raw, usable);

    const u32 ec = Le32(work);
    if (ec == 0)
        return false;

    // The LCG stream covers the four shuffled blocks, then the party stats with a fresh seed.
    u32 seed = ec;
    for (std::size_t i = 8; i + 1 < kStoredSize; i += 2) {
        seed = 0x41C64E6Du * seed + 0x00006073u;
        const u16 xorv = static_cast<u16>(seed >> 16);
        work[i] ^= static_cast<u8>(xorv & 0xFF);
        work[i + 1] ^= static_cast<u8>(xorv >> 8);
    }
    if (usable > kStoredSize) {
        seed = ec;
        for (std::size_t i = kStoredSize; i + 1 < usable; i += 2) {
            seed = 0x41C64E6Du * seed + 0x00006073u;
            const u16 xorv = static_cast<u16>(seed >> 16);
            work[i] ^= static_cast<u8>(xorv & 0xFF);
            work[i + 1] ^= static_cast<u8>(xorv >> 8);
        }
    }

    // Un-shuffle the four 0x50-byte blocks.
    const u32 sv = (ec >> 13) & 31;
    const u8* shuffle = kBlockPosition + (sv * 4);
    u8 perm[4] = {0, 1, 2, 3};
    u8 slot_of[4] = {0, 1, 2, 3};
    for (u8 i = 0; i < 3; ++i) {
        const u8 desired = shuffle[i];
        const u8 j = slot_of[desired];
        if (j == i)
            continue;
        u8* a = work + 8 + (i * 0x50);
        u8* b = work + 8 + (j * 0x50);
        for (std::size_t k = 0; k < 0x50; ++k)
            std::swap(a[k], b[k]);
        const u8 block_at_i = perm[i];
        perm[j] = block_at_i;
        slot_of[block_at_i] = j;
    }

    return ReadPlainEntity(work, usable, out);
}

bool ReadPlainEntity(const u8* work, std::size_t usable, Entity& out) {
    out = Entity{};
    if (!work || usable < kStoredSize) return false;
    out.internal_species = Le16(work + 0x08);
    if (out.internal_species == 0)
        return false;
    out.present = true;
    out.encryption_constant = Le32(work);
    out.pid = Le32(work + 0x1C);
    out.checksum = Le16(work + 6);
    u16 computed_checksum = 0;
    for (std::size_t i = 8; i < kStoredSize; i += 2)
        computed_checksum = static_cast<u16>(computed_checksum + Le16(work + i));
    out.checksum_ok = Le16(work + 4) == 0 && computed_checksum == out.checksum;
    out.alpha = work[0x23] != 0;
    out.status_condition = Le32(work + 0x90);
    out.ability = Le16(work + 0x14);
    for (std::size_t i = 0; i < 4; ++i) {
        out.moves[i] = Le16(work + 0x72 + i * 2);
        out.move_pp[i] = work[0x7A + i];
        out.move_pp_ups[i] = work[0x7E + i];
    }
    const u32 iv_bits = Le32(work + 0x8C);
    out.egg = (iv_bits & (1u << 30)) != 0;
    for (std::size_t i = 0; i < 6; ++i) {
        out.evs[i] = work[0x26 + i];
        out.ivs[i] = static_cast<u8>((iv_bits >> (5 * i)) & 31);
    }
    out.species = NationalSpecies(out.internal_species);
    out.held_item = Le16(work + 0x0A);
    out.exp = Le32(work + 0x10);
    out.nature = work[0x20];
    out.gender = static_cast<u8>((work[0x22] >> 1) & 0x3);
    out.form = work[0x24];
    out.nickname = Utf16ToUtf8(work + 0x58, 26);
    out.ball = work[0x124];
    out.hp = Le16(work + 0x8A);
    if (usable >= kPartySize) {
        out.level = work[0x148];
        out.hp_max = Le16(work + 0x14A);
        out.attack = Le16(work + 0x14C);
        out.defense = Le16(work + 0x14E);
        out.speed = Le16(work + 0x150);
        out.sp_attack = Le16(work + 0x152);
        out.sp_defense = Le16(work + 0x154);
    }

    // Gen 8/9 shininess uses the personality value, independently of the encryption constant.
    const u16 tid = Le16(work + 0x0C);
    const u16 sid = Le16(work + 0x0E);
    const u32 pid = Le32(work + 0x1C);
    const u16 mixed = static_cast<u16>((pid & 0xFFFF) ^ (pid >> 16) ^ tid ^ sid);
    out.shiny = mixed < 16;
    return true;
}

u16 NationalSpecies(u16 internal) {
    // The Gen-9 internal order only diverges from the National Dex at 917.
    constexpr u16 kFirstUnaligned = 917;
    static constexpr signed char kDelta[] = {
        65,  -1,  -1,  -1,  -1,  31,  31,  47,  47,  29,  29,  53,  31,  31,  46,  44,  30,  30,
        -7,  -7,  -7,  13,  13,  -2,  -2,  23,  23,  24,  -21, -21, 27,  27,  47,  47,  47,  26,
        14,  -33, -33, -33, -17, -17, 3,   -29, 12,  -12, -31, -31, -31, 3,   3,   -24, -24, -44,
        -44, -30, -30, -28, -28, 23,  23,  6,   7,   29,  8,   3,   4,   4,   20,  4,   23,  6,
        3,   3,   4,   -1,  13,  9,   7,   5,   7,   9,   9,   -43, -43, -43, -68, -68, -68, -58,
        -58, -25, -29, -31, 6,   -1,  6,   0,   0,   0,   3,   3,   4,   2,   3,   3,   -5,  -12,
        -12,
    };
    if (internal < kFirstUnaligned)
        return internal;
    const std::size_t index = internal - kFirstUnaligned;
    if (index >= sizeof(kDelta))
        return internal;
    return static_cast<u16>(internal + kDelta[index]);
}

int GenerationOf(u16 national) {
    if (national < 1 || national > 1025)
        return 0;
    static constexpr u16 kLast[9] = {151, 251, 386, 493, 649, 721, 809, 905, 1025};
    for (int gen = 0; gen < 9; ++gen) {
        if (national <= kLast[gen])
            return gen + 1;
    }
    return 0;
}

std::string Utf16ToUtf8(const u8* bytes, std::size_t size) {
    std::string out;
    if (!bytes)
        return out;
    for (std::size_t i = 0; i + 1 < size; i += 2) {
        u32 cp = Le16(bytes + i);
        if (cp == 0)
            break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < size) {
            const u32 low = Le16(bytes + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

namespace {

std::string AsciiField(const u8* data, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size && data[i] != 0; ++i)
        out.push_back(static_cast<char>(data[i]));
    return out;
}

} // namespace

Save Parse(const std::vector<u8>& bytes, int box_index) {
    Save save;
    save.file_size = bytes.size();
    if (bytes.size() < kHashSize + 16) {
        save.status = "sauvegarde absente";
        return save;
    }

    const std::size_t payload_size = bytes.size() - kHashSize;
    u8 digest[32];
    crypto::Sha256Of3(kIntro, sizeof(kIntro), bytes.data(), payload_size, kOutro, sizeof(kOutro),
                      digest);
    save.hash_ok = std::memcmp(digest, bytes.data() + payload_size, sizeof(digest)) == 0;

    // Decrypt a copy: the payload minus the trailer, XORed with the repeating pad.
    std::vector<u8> payload(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(payload_size));
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] ^= kXorPad[i % sizeof(kXorPad)];

    std::vector<BlockView> blocks;
    if (!ReadBlocks(payload, blocks)) {
        save.status = "format de sauvegarde inconnu";
        return save;
    }
    save.valid = true;
    save.block_count = blocks.size();

    if (const auto* b = FindBlock(blocks, KMyStatus); b && b->size >= 0x2A) {
        save.trainer.id32 = Le32(b->data);
        save.trainer.tid = Le16(b->data);
        save.trainer.sid = Le16(b->data + 2);
        save.trainer.game = b->data[4];
        save.trainer.gender = b->data[5];
        save.trainer.language = b->data[7];
        save.trainer.name = Utf16ToUtf8(b->data + 0x10, 0x1A);
    }
    if (const auto* b = FindBlock(blocks, KMoney); b && b->size >= 4)
        save.money = Le32(b->data);
    if(const auto* b=FindBlock(blocks,0x85DBDCE9);b&&b->size>=12){save.research_level=int(Le32(b->data));save.research_points=int(Le32(b->data+8));}
    if(const auto* b=FindBlock(blocks,0x0235471C);b&&b->size>=4)save.hyperspace_points=int(Le32(b->data));
    if(const auto* b=FindBlock(blocks,0x9A730DE1);b&&b->size>=4)save.ticket_points=int(Le32(b->data));
    if(const auto* b=FindBlock(blocks,0x1D7EE369);b&&b->size>=4)save.infinite_points=int(Le32(b->data));
    if(const auto* b=FindBlock(blocks,0x8929BFB6);b&&b->size>=16){save.royale_wins=int(Le32(b->data+8));save.royale_medals=int(Le32(b->data+12));}
    if(const auto* b=FindBlock(blocks,0xFADA7742);b&&b->size<=4096&&b->size%16==0)for(std::size_t i=0;i<b->size;i+=16)save.system_work[Le64(b->data+i)]=Le64(b->data+i+8);
    if (const auto* b = FindBlock(blocks, KPlayedSeconds); b && b->size >= 8) {
        const double seconds = LeF64(b->data);
        save.play_seconds = seconds > 0.0 ? static_cast<u32>(seconds) : 0;
    }
    if (const auto* b = FindBlock(blocks, KTimeSecondsPastMidnight); b && b->size >= 4)
        save.day_seconds = LeF32(b->data);
    if (const auto* b = FindBlock(blocks, KBoxesUnlocked); b && b->size >= 1)
        save.boxes_unlocked = b->data[0];
    if (const auto* b = FindBlock(blocks, KCurrentBox); b && b->size >= 1)
        save.current_box = b->data[0];

    if (const auto* b = FindBlock(blocks, KParty); b) {
        const std::size_t slots = std::min<std::size_t>(kPartySlots, b->size / kPartySlotSize);
        int count = 0;
        for (std::size_t i = 0; i < slots; ++i) {
            Entity entity;
            if (DecryptEntity(b->data + (i * kPartySlotSize), kPartySize, entity)) {
                if (entity.species < save.owned.size()) ++save.owned[entity.species];
                save.party[i] = std::move(entity);
                ++count;
            }
        }
        save.party_count = count;
    }

    if (const auto* b = FindBlock(blocks, KBox); b) {
        auto entries=std::make_shared<std::vector<Save::BoxEntry>>();
        const std::size_t slots = b->size / kBoxSlotSize;
        // Clamp the requested box to what this save actually holds.
        const int available = slots >= kBoxSlotCount ? static_cast<int>(slots / kBoxSlotCount) : 0;
        save.boxes_available = std::min(available, static_cast<int>(kBoxCount));
        const int wanted = box_index < 0 ? save.current_box : box_index;
        save.box_view_index =
            save.boxes_available > 0 ? std::clamp(wanted, 0, save.boxes_available - 1) : 0;
        for (std::size_t i = 0; i < slots; ++i) {
            const int box = static_cast<int>(i / kBoxSlotCount);
            const int slot = static_cast<int>(i % kBoxSlotCount);
            if (Le32(b->data + (i * kBoxSlotSize)) == 0)
                continue;
            Entity entity;
            if (!DecryptEntity(b->data + (i * kBoxSlotSize), kPartySize, entity))
                continue;
            if (entity.species < save.owned.size()) ++save.owned[entity.species];
            ++save.box_used;
            if (box >= 0 && box < static_cast<int>(kBoxCount)) {
                ++save.boxes_used[box];
                entries->push_back({u16(box),u16(slot),entity.internal_species,entity.form,
                                    entity.pid,entity.encryption_constant,entity.nickname,entity.gender,entity.egg});
            }
            if (box == save.box_view_index)
                save.box_view[slot] = std::move(entity);
        }
        save.box_entries=std::move(entries);
    }

    if (const auto* b = FindBlock(blocks, KPokedex); b) {
        const std::size_t entries = std::min<std::size_t>(kDexEntries, b->size / kDexEntrySize);
        for (std::size_t i = 0; i < entries; ++i) {
            // Dex entries are indexed by the game's *internal* species id.
            const u16 national = NationalSpecies(static_cast<u16>(i));
            const int gen = GenerationOf(national);
            if (gen == 0)
                continue;
            const u8* entry = b->data + (i * kDexEntrySize);
            if (Le32(entry) != 0) {
                save.dex_state[national] = 2;
                ++save.dex_caught;
                ++save.dex_caught_by_gen[gen - 1];
            } else if (Le32(entry + 4) != 0) {
                save.dex_state[national] = 1;
                ++save.dex_seen;
            }
        }
    }

    // Inventory records are indexed by item id, 16 bytes each. The tail contains pouch flags,
    // so stop at the documented 3000 item records even when the block has additional bytes.
    if (const auto* b = FindBlock(blocks, KItem); b) {
        const auto count = std::min<std::size_t>(3000, b->size / 16);
        for (std::size_t i = 1; i < count; ++i) {
            const auto* entry = b->data + i * 16;
            const u32 pouch = Le32(entry), quantity = Le32(entry + 4), flags = Le32(entry + 8);
            if (pouch > 8 || quantity > 0x7FFFFFFFu || (quantity == 0 && !(flags & 16))) continue;
            save.items.push_back({static_cast<u16>(i), quantity, pouch, flags});
        }
    }

    for(auto work_key:{0xB9B223B9u,0x03913534u})if(const auto* b=FindBlock(blocks,work_key);b&&b->size%16==0&&b->size<=16384){
        for(std::size_t at=0;at<b->size;at+=16){auto key=Le64(b->data+at);if(key!=14695981039346656837ull)save.quest_work[key]=Le64(b->data+at+8);}
    }
    if (const auto* b = FindBlock(blocks, KCoordinates); b && b->size >= 0x30) {
        // 0x00: short area id ("t1"), 0x20: x, 0x24: y (height), 0x28: z, 0x2C: rotation.
        save.map_name = AsciiField(b->data, 0x20);
        save.x = LeF32(b->data + 0x20);
        save.y = LeF32(b->data + 0x24);
        save.z = LeF32(b->data + 0x28);
        save.rotation = LeF32(b->data + 0x2C);
    }
    if (const auto* b = FindBlock(blocks, KStreetName); b && b->size >= 2)
        save.street = Utf16ToUtf8(b->data, b->size);

    if (save.hash_ok)
        save.status = "sauvegarde lue";
    else
        save.status = "somme de controle invalide";
    return save;
}

} // namespace za
