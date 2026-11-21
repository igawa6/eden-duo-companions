// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Offline regression for the Legends Z-A save reader.
//
// The test writes the container and the entities itself (the same SwishCrypto and PA9 layouts the
// module reads), then checks that Parse() recovers every field. That proves the two decoders are
// self-consistent; it does not pretend to prove the layout matches a real save -- the module's
// log line reports "hash ok" the first time a player's own save is read, which is that check.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "za_save.h"
#include "za_sha256.h"

namespace {

using za::u16;
using za::u32;
using za::u64;
using za::u8;

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

constexpr u8 kBlockPositionInvert[32] = {
    0, 1, 2, 4, 3, 5, 6, 7, 12, 18, 13, 19, 8, 10, 14, 20,
    16, 22, 9, 11, 15, 21, 17, 23, 0, 1, 2, 4, 3, 5, 6, 7,
};

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

void Put16(std::vector<u8>& v, std::size_t at, u16 value) {
    v[at] = static_cast<u8>(value & 0xFF);
    v[at + 1] = static_cast<u8>(value >> 8);
}
void Put32(std::vector<u8>& v, std::size_t at, u32 value) {
    for (int i = 0; i < 4; ++i)
        v[at + static_cast<std::size_t>(i)] = static_cast<u8>(value >> (8 * i));
}
void PutFloat(std::vector<u8>& v, std::size_t at, float f) {
    u32 bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    Put32(v, at, bits);
}
void PutDouble(std::vector<u8>& v, std::size_t at, double d) {
    u64 bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    for (int i = 0; i < 8; ++i)
        v[at + static_cast<std::size_t>(i)] = static_cast<u8>(bits >> (8 * i));
}
void PutUtf16(std::vector<u8>& v, std::size_t at, const std::string& text, std::size_t bytes) {
    std::size_t i = 0;
    for (; i < text.size() && (i + 1) * 2 <= bytes; ++i)
        Put16(v, at + (i * 2), static_cast<u16>(static_cast<u8>(text[i])));
}

// Encrypts one PA9 party entity the way the game stores it.
std::vector<u8> EncryptEntity(std::vector<u8> plain, u32 ec) {
    Put32(plain, 0, ec);
    const u32 sv = (ec >> 13) & 31;
    const u8 inverse = kBlockPositionInvert[sv];
    const u8* shuffle = kBlockPosition + (inverse * 4);
    u8 perm[4] = {0, 1, 2, 3};
    u8 slot_of[4] = {0, 1, 2, 3};
    for (u8 i = 0; i < 3; ++i) {
        const u8 desired = shuffle[i];
        const u8 j = slot_of[desired];
        if (j == i)
            continue;
        u8* a = plain.data() + 8 + (i * 0x50);
        u8* b = plain.data() + 8 + (j * 0x50);
        for (std::size_t k = 0; k < 0x50; ++k)
            std::swap(a[k], b[k]);
        const u8 block_at_i = perm[i];
        perm[j] = block_at_i;
        slot_of[block_at_i] = j;
    }
    // Two independent LCG streams, both seeded with the encryption constant: the stored blocks,
    // then the party stats.
    u32 seed = ec;
    for (std::size_t i = 8; i + 1 < za::kStoredSize; i += 2) {
        seed = 0x41C64E6Du * seed + 0x00006073u;
        const u16 xorv = static_cast<u16>(seed >> 16);
        plain[i] ^= static_cast<u8>(xorv & 0xFF);
        plain[i + 1] ^= static_cast<u8>(xorv >> 8);
    }
    seed = ec;
    for (std::size_t i = za::kStoredSize; i + 1 < plain.size(); i += 2) {
        seed = 0x41C64E6Du * seed + 0x00006073u;
        const u16 xorv = static_cast<u16>(seed >> 16);
        plain[i] ^= static_cast<u8>(xorv & 0xFF);
        plain[i + 1] ^= static_cast<u8>(xorv >> 8);
    }
    return plain;
}

void AppendBlock(std::vector<u8>& payload, u32 key, const std::vector<u8>& data) {
    for (int i = 0; i < 4; ++i)
        payload.push_back(static_cast<u8>(key >> (8 * i)));
    XorShift32 xk(key);
    payload.push_back(static_cast<u8>(4 ^ xk.Next()));
    const u32 length = static_cast<u32>(data.size());
    const u32 enc_length = length ^ xk.Next32();
    for (int i = 0; i < 4; ++i)
        payload.push_back(static_cast<u8>(enc_length >> (8 * i)));
    for (u8 byte : data)
        payload.push_back(static_cast<u8>(byte ^ xk.Next()));
}

std::vector<u8> BuildSave() {
    // Trainer.
    std::vector<u8> status(0x78, 0);
    Put32(status, 0x00, 0x12345678u); // ID32
    Put16(status, 0x00, 0x5678u);     // TID
    Put16(status, 0x02, 0x1234u);     // SID
    status[0x04] = 51;                // game
    status[0x05] = 1;                 // gender
    status[0x07] = 2;                 // language
    PutUtf16(status, 0x10, "Liko", 0x1A);

    std::vector<u8> money(4, 0);
    Put32(money, 0, 123456u);

    std::vector<u8> played(8, 0);
    PutDouble(played, 0, 3725.75); // 1 h 02 min 05 s

    // Party: two entities.
    std::vector<u8> party(0xB40, 0);
    {
        std::vector<u8> mon(0x158, 0);
        Put16(mon, 0x08, 25); // Pikachu (internal == national below 917)
        Put16(mon, 0x0A, 12); // held item
        Put16(mon, 0x0C, 0x5678u);
        Put16(mon, 0x0E, 0x1234u);
        Put32(mon, 0x1C, 0x12345668u); // PID xor TID/SID is 16: not shiny
        mon[0x20] = 3;  // nature
        mon[0x22] = 2;  // gender bits: female
        mon[0x58] = 'P';
        mon[0x5A] = 'i';
        mon[0x5C] = 'k';
        mon[0x5E] = 'a';
        Put16(mon, 0x8A, 42); // HP
        mon[0x148] = 17;      // level
        Put16(mon, 0x14A, 60);
        Put16(mon, 0x14C, 31);
        Put16(mon, 0x14E, 22);
        Put16(mon, 0x150, 44);
        Put16(mon, 0x152, 33);
        Put16(mon, 0x154, 28);
        const auto stored = EncryptEntity(mon, 0xA1B2C3D4u);
        std::memcpy(party.data(), stored.data(), stored.size());
    }
    {
        std::vector<u8> mon(0x158, 0);
        Put16(mon, 0x08, 1000); // an internal id past 917 (delta table)
        mon[0x20] = 5;
        mon[0x58] = 'X';
        mon[0x5A] = 'e';
        mon[0x5C] = 'r';
        mon[0x148] = 55;
        Put16(mon, 0x14A, 200);
        Put16(mon, 0x8A, 200);
        const auto stored = EncryptEntity(mon, 0x0BADF00Du);
        std::memcpy(party.data() + 0x1E0, stored.data(), stored.size());
    }

    // Box: one entity in box 0, slot 3.
    std::vector<u8> box(0x5FA00, 0);
    {
        std::vector<u8> mon(0x158, 0);
        Put16(mon, 0x08, 133);
        mon[0x58] = 'E';
        mon[0x5A] = 'e';
        mon[0x148] = 12;
        const auto stored = EncryptEntity(mon, 0x11223344u);
        std::memcpy(box.data() + (3 * 0x198), stored.data(), stored.size());
    }

    // Pokédex: two caught, one seen only.
    std::vector<u8> dex(0x27068, 0);
    Put32(dex, 25 * 0x84, 1);
    Put32(dex, 6 * 0x84, 1);
    Put32(dex, 150 * 0x84 + 4, 1);

    std::vector<u8> inventory(0xBC00, 0);
    Put32(inventory, 4*16, 1); Put32(inventory, 4*16+4, 25); Put32(inventory, 4*16+8, 2);
    Put32(inventory, 17*16, 0); Put32(inventory, 17*16+4, 3);
    Put32(inventory, 18*16, 7); Put32(inventory, 18*16+8, 16); // equipped, count zero
    Put32(inventory, 10*16, 0xFFFFFFFFu); Put32(inventory, 10*16+4, 99); // invalid pouch
    Put32(inventory, 3000*16, 1); Put32(inventory, 3000*16+4, 99); // tail, not an item

    // Coordinates: map name + position.
    std::vector<u8> coords(0x70, 0);
    std::memcpy(coords.data(), "a0101", 5);
    PutFloat(coords, 0x20, 12.5f);  // x
    PutFloat(coords, 0x24, 3.75f);  // y (height)
    PutFloat(coords, 0x28, -34.25f); // z
    PutFloat(coords, 0x2C, 1.5f);   // rotation

    std::vector<u8> layout(0x440, 0);
    std::vector<u8> revision(8, 0);

    std::vector<u8> payload;
    AppendBlock(payload, za::KMyStatus, status);
    AppendBlock(payload, za::KMoney, money);
    AppendBlock(payload, za::KPlayedSeconds, played);
    AppendBlock(payload, za::KParty, party);
    AppendBlock(payload, za::KBox, box);
    AppendBlock(payload, za::KPokedex, dex);
    AppendBlock(payload, za::KItem, inventory);
    AppendBlock(payload, za::KCoordinates, coords);
    AppendBlock(payload, za::KBoxLayout, layout);
    AppendBlock(payload, za::KSaveRevision, revision);

    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] ^= kXorPad[i % sizeof(kXorPad)];

    std::vector<u8> file = payload;
    file.resize(payload.size() + 0x20);
    za::crypto::Sha256Of3(kIntro, sizeof(kIntro), payload.data(), payload.size(), kOutro,
                          sizeof(kOutro), file.data() + payload.size());
    return file;
}

} // namespace

int main() {
    const auto file = BuildSave();
    const za::Save save = za::Parse(file);

    assert(save.valid);
    assert(save.hash_ok);
    assert(save.block_count == 10);
    assert(save.trainer.id32 == 0x12345678u);
    assert(save.trainer.tid == 0x5678u);
    assert(save.trainer.sid == 0x1234u);
    assert(save.trainer.gender == 1);
    assert(save.trainer.language == 2);
    assert(save.trainer.name == "Liko");
    assert(save.money == 123456u);
    assert(save.play_seconds == 3725u);
    assert(save.party_count == 2);

    const za::Entity& first = save.party[0];
    assert(first.present);
    assert(first.species == 25);
    assert(first.nickname == "Pika");
    assert(first.level == 17);
    assert(first.hp == 42);
    assert(first.hp_max == 60);
    assert(first.attack == 31);
    assert(first.defense == 22);
    assert(first.speed == 44);
    assert(first.sp_attack == 33);
    assert(first.sp_defense == 28);
    assert(first.gender == 1);
    assert(first.nature == 3);
    assert(first.held_item == 12);
    assert(!first.shiny); // PID/TID/SID xor is 16

    const za::Entity& second = save.party[1];
    assert(second.present);
    assert(second.internal_species == 1000);
    assert(second.species == za::NationalSpecies(1000));
    assert(second.species != 1000); // the delta table moved it
    assert(second.nickname == "Xer");
    assert(second.level == 55);
    assert(second.shiny); // PID/TID/SID are zero, while EC is deliberately nonzero

    assert(save.box_used == 1);
    assert(save.boxes_available == 32);
    assert(save.boxes_used[0] == 1);
    assert(save.box_view_index == 0);
    assert(save.box_view[3].present);
    assert(save.box_view[3].species == 133);
    assert(save.box_view[3].nickname == "Ee");
    assert(save.box_view[3].level == 12);
    assert(!save.box_view[0].present);
    assert(save.dex_state[25] == 2 && save.dex_state[150] == 1 && save.dex_state[1] == 0);
    assert(save.owned[25] == 1 && save.owned[133] == 1 && save.owned[6] == 0);
    assert(save.items.size() == 3);
    assert(save.items[0].id == 4 && save.items[0].count == 25 && save.items[0].pouch == 1);
    assert(save.items[0].flags == 2);
    assert(save.items[2].id == 18 && save.items[2].count == 0 && (save.items[2].flags & 16));
    assert(save.dex_caught == 2);
    assert(save.dex_seen == 1);
    assert(save.dex_caught_by_gen[0] == 2);
    assert(save.dex_caught_by_gen[1] == 0);

    // Asking for another box shows that box instead (here: empty), and the range is clamped.
    const za::Save other = za::Parse(file, 1);
    assert(other.box_view_index == 1);
    assert(other.boxes_used[1] == 0);
    for (const auto& entity : other.box_view)
        assert(!entity.present);
    const za::Save clamped = za::Parse(file, 999);
    assert(clamped.box_view_index == 31);
    assert(save.map_name == "a0101");
    assert(save.x == 12.5f);
    assert(save.z == -34.25f);
    assert(save.y == 3.75f);
    assert(save.rotation == 1.5f);

    // A torn write (one byte changed) is still parsed, but flagged.
    auto broken = file;
    broken[0x40] ^= 0xFF;
    const za::Save torn = za::Parse(broken);
    assert(torn.valid);
    assert(!torn.hash_ok);
    assert(torn.status == "somme de controle invalide");

    // A file that is not a container at all is refused, not half-read.
    std::vector<u8> garbage(0x400, 0xCC);
    const za::Save refused = za::Parse(garbage);
    assert(!refused.valid);
    assert(!refused.hash_ok);

    std::printf("za-save: ok (%zu blocks, party %d, dex %d/%d, box %d)\n", save.block_count,
                save.party_count, save.dex_caught, save.dex_seen, save.box_used);
    return 0;
}
