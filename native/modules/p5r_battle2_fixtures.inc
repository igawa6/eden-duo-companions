// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic battle2 fixtures (generated). Nothing here is copied from the game:
//   - Battle2CodeFixture: for each p5r_battle2::Code range, LCG filler bytes whose last 16
//     bytes are solved (lattice reduction over the FNV-1a steps) so that FNV-1a-64 of the range
//     equals the reader's fingerprint. The reader only hashes these ranges; it matches no word
//     inside them, so no instruction encoding is needed.
//   - Battle2Fixture: made-up battle states (unit fields, actors, affinity rows, discovery masks,
//     camera actors) laid out as p5r_battle2_reader.h reads them; InstallBattle2 in
//     01005CA01580E000_test.cpp builds guest memory from them.
struct Battle2Code {
    uint64_t offset;
    std::vector<uint8_t> bytes;
};
// Filler for a range: bytes of a 64-bit LCG seeded with `salt` (top byte of each step).
inline std::vector<uint8_t> Battle2Filler(uint64_t salt, size_t n) {
    std::vector<uint8_t> b(n);
    for (auto& v : b) {
        salt = salt * 6364136223846793005ULL + 1442695040888963407ULL;
        v = static_cast<uint8_t>(salt >> 56);
    }
    return b;
}
// {offset, size, filler salt, solved last bytes}: the range = Battle2Filler(salt, size - n) +
// the n solved bytes (the 12-byte range is solved whole).
struct Battle2CodeSpec {
    uint64_t offset, size, salt;
    std::vector<uint8_t> tail;
};
inline const std::vector<Battle2Code>& Battle2CodeFixture() {
    static const std::vector<Battle2Code> v = [] {
        const std::vector<Battle2CodeSpec> specs{
            {0x8a3e10,
             0x930,
             1,
             {0x3f, 0x3c, 0x0d, 0x0f, 0x00, 0x08, 0x04, 0x01, 0x01, 0x02, 0x02, 0x0e, 0x07, 0x01,
              0x05, 0x02}},
            {0x15ea728,
             0x24,
             1,
             {0x07, 0x0e, 0x1b, 0x19, 0x05, 0x34, 0x7b, 0x1d, 0x1a, 0x1e, 0x0c, 0x01, 0x0c, 0x0a,
              0x1b, 0x3e}},
            {0x872b70,
             0x50,
             1,
             {0x04, 0x38, 0x0d, 0x06, 0x1f, 0x01, 0x0c, 0x7d, 0x06, 0x1b, 0x7f, 0x0f, 0x01, 0x00,
              0x05, 0x00}},
            {0x89b820,
             0x20,
             1,
             {0x1b, 0x7f, 0x02, 0x3d, 0x19, 0x01, 0x03, 0x1e, 0x09, 0x03, 0x3a, 0x00, 0x0d, 0x1d,
              0x00, 0x04}},
            {0x89b7a0,
             0x18,
             1,
             {0x00, 0x06, 0x3a, 0x06, 0x01, 0x08, 0x3c, 0x00, 0x0e, 0x05, 0x00, 0x3f, 0x0e, 0x05,
              0x00, 0x0a}},
            {0x8a3660,
             0x150,
             1,
             {0x01, 0x04, 0x00, 0x0a, 0x0a, 0x02, 0x1c, 0x02, 0x1f, 0x08, 0x0b, 0x03, 0x06, 0x17,
              0x09, 0x0d}},
            {0x8725b0,
             0x44,
             1,
             {0x3e, 0x07, 0x09, 0x02, 0x35, 0x17, 0x3f, 0x05, 0x39, 0x1f, 0x01, 0x02, 0x07, 0x01,
              0x17, 0x05}},
            {0x8808b0,
             0xc,
             0,
             {0x07, 0x77, 0x03, 0x66, 0x19, 0x07, 0x77, 0x18, 0x37, 0x0d, 0x1a, 0x30}},
            {0x701c80,
             0x47c,
             1,
             {0x01, 0x79, 0x0b, 0x08, 0x05, 0x05, 0x06, 0x07, 0x15, 0x07, 0x01, 0x00, 0x02, 0x7c,
              0x05, 0x01}},
            {0x7010a8,
             0x34,
             1,
             {0x0d, 0x05, 0x07, 0x01, 0x05, 0x1e, 0x0c, 0x1d, 0x3c, 0x07, 0x0b, 0x1c, 0x02, 0x05,
              0x1a, 0x1d}},
            {0x677a70,
             0x10,
             0,
             {0x01, 0x04, 0x04, 0x05, 0x01, 0x0c, 0x36, 0x78, 0x7e, 0x05, 0x05, 0x0e, 0x06, 0x00,
              0x04, 0x06}},
            {0x4012ec,
             0x24,
             1,
             {0x09, 0x01, 0x0a, 0x00, 0x3b, 0x02, 0x02, 0x0c, 0x1b, 0x1b, 0x08, 0x06, 0x0d, 0x01,
              0x07, 0x0d}},
            {0x4fec74,
             0x40,
             1,
             {0x0e, 0x3c, 0x02, 0x00, 0x07, 0x0f, 0x05, 0x02, 0x02, 0x0f, 0x3d, 0x0c, 0x00, 0x7e,
              0x05, 0x04}},
            {0x1f8e5c,
             0x14,
             1,
             {0x03, 0x03, 0x0a, 0x00, 0x01, 0x0f, 0x00, 0x04, 0x02, 0x04, 0x07, 0x1c, 0x0f, 0x07,
              0x09, 0x1e}},
            {0x3ad934,
             0x40,
             1,
             {0x0c, 0x0d, 0x03, 0x03, 0x03, 0x07, 0x1b, 0x0f, 0x18, 0x05, 0x07, 0x1d, 0x03, 0x00,
              0x06, 0x01}},
            {0x1c80c,
             0x3c,
             1,
             {0x0f, 0x02, 0x02, 0x0b, 0x00, 0x08, 0x03, 0x0c, 0x19, 0x19, 0x01, 0x06, 0x06, 0x1e,
              0x00, 0x03}},
            {0x15fc,
             0x20,
             1,
             {0x1b, 0x04, 0x09, 0x1c, 0x04, 0x3f, 0x03, 0x05, 0x0c, 0x1c, 0x01, 0x00, 0x05, 0x04,
              0x00, 0x03}},
            {0x1c7d0,
             0x3c,
             1,
             {0x0c, 0x00, 0x1a, 0x05, 0x13, 0x00, 0x00, 0x0d, 0x00, 0x06, 0x05, 0x03, 0x03, 0x03,
              0x0d, 0x07}},
        };
        std::vector<Battle2Code> out;
        for (const auto& s : specs) {
            auto bytes = Battle2Filler(s.salt, s.size - s.tail.size());
            bytes.insert(bytes.end(), s.tail.begin(), s.tail.end());
            out.push_back({s.offset, std::move(bytes)});
        }
        return out;
    }();
    return v;
}
// A battle unit as the battle lists see it: actor (vtable, flags +8, state handler +0x230/+0x238)
// and the unit fields the readers use (u32 +0 flags, u16 +4 kind, u32 +8 id, +0xC HP, +0x10 SP,
// +0x14 status, u16 +0x18 level). state_fn is main-relative (0x1C848 idle, 0xCD1A0 choosing a
// command, 0x1408 incapacitated, anything else acting).
struct Battle2Unit {
    uint64_t actor, actor_flags, state_fn, state_adj;
    uint16_t kind, level;
    uint32_t id, hp, sp, status, flags;
};
// Enemy table rows: record (max HP +8, max SP +0xC, skills +0x16), affinity row (20 u16) and the
// discovered elements (bit e = element e known: discovery bit id*11+e).
struct Battle2Enemy {
    uint32_t id, max_hp, max_sp;
    std::array<uint16_t, 8> skills;
    std::array<uint16_t, 20> row;
    uint16_t known;
};
struct Battle2Fixture {
    std::vector<Battle2Unit> party, enemies;
    uint64_t camera_acting, camera_input; // camera +0x98 / +0xB8
    std::vector<uint64_t> targets;        // camera target list (actors)
    std::vector<Battle2Enemy> tables;
    std::array<uint16_t, 10> order;    // displayed element order
    std::vector<uint16_t> party_array; // units+0x1CE0 (HUD order)
};
// Affinity row words (872B70): low byte * 5 = percentage, high byte = flags; with a zero low byte
// 0x800 -> weak 125, 0x700 -> 100, 0x1000 -> resist 50, else 100.
constexpr uint16_t AffNormal = 0x0014, AffNormalPct = 0x0019, AffFlagged = 0x4000, AffWeak = 0x0800,
                   AffResist = 0x1000, AffNull = 0x0100, AffHigh = 0x8000, AffWeakPct = 0x0832;
constexpr std::array<uint16_t, 10> ElementOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
// One enemy (id 321), four party members (ids 1, 4, 7, 10); member 4 (party slot 1) is choosing
// a command and the camera target list holds the enemy.
inline const Battle2Fixture& Fixture_single_input() {
    static const Battle2Fixture f{
        {{0x2100010000, 0, 0xcd1a0, 0, 1, 30, 4, 250, 120, 0, 0},
         {0x2100020000, 0, 0x1c848, 0, 1, 29, 7, 180, 95, 0, 0},
         {0x2100030000, 0, 0x1c848, 0, 1, 28, 10, 205, 140, 0, 0},
         {0x2100040000, 0, 0x1c848, 0, 1, 31, 1, 300, 200, 0, 0}},
        {{0x2100050000, 0, 0x1c848, 0, 2, 60, 321, 2100, 900, 0, 0x8001}},
        0,
        0x2100010000,
        {0x2100050000},
        {{321,
          2400,
          900,
          {0x21, 0x42, 0x63},
          {AffNormal, AffNormalPct, AffFlagged, AffWeak,   AffResist, AffNull,   AffNormal,
           AffNormal, AffNormal,    AffNormal,  AffNormal, AffNormal, AffNormal, AffNormal,
           AffNormal, AffNormal,    AffNormal,  AffNormal, AffNormal, AffNormal},
          0x14f}}, // elements 0, 1, 2, 3, 6, 8 discovered
        ElementOrder,
        {1, 4, 7, 10}};
    return f;
}
// The same battle with the enemy acting (handler 0x2468) on member 7 (party slot 2).
inline const Battle2Fixture& Fixture_single_enemy() {
    static const Battle2Fixture f = [] {
        Battle2Fixture g = Fixture_single_input();
        g.party[0].state_fn = 0x1c848;
        g.enemies[0].state_fn = 0x2468;
        g.camera_acting = g.enemies[0].actor;
        g.targets = {g.party[1].actor};
        return g;
    }();
    return f;
}
// Three enemies: id 55 and two of id 88 (same table rows). Battle list order 3, 2, 4, 1 differs
// from the HUD order 1, 3, 2, 4; member 2 (HUD slot 2) is choosing, the cursor is on enemy row 1.
inline const Battle2Fixture& Fixture_three() {
    static const Battle2Fixture f{
        {{0x2200010000, 0, 0x1c848, 0, 1, 11, 3, 140, 60, 0, 0},
         {0x2200020000, 0, 0xcd1a0, 0, 1, 12, 2, 132, 48, 0, 0},
         {0x2200030000, 0, 0x1c848, 0, 1, 12, 4, 150, 70, 0, 0},
         {0x2200040000, 0, 0x1c848, 0, 1, 13, 1, 170, 80, 0, 0}},
        {{0x2200050000, 0, 0x1c848, 0, 2, 12, 55, 400, 60, 0, 0x8001},
         {0x2200060000, 0, 0x1c848, 0, 2, 7, 88, 90, 20, 0, 0x8001},
         {0x2200070000, 0, 0x1c848, 0, 2, 7, 88, 150, 20, 0, 0x8001}},
        0,
        0x2200020000,
        {0x2200060000},
        {{55,
          480,
          60,
          {0x10},
          {AffResist, AffNormal, AffWeak,   AffHigh,   AffNull,   AffNormalPct, AffNormal,
           AffNormal, AffNormal, AffNormal, AffNormal, AffNormal, AffNormal,    AffNormal,
           AffNormal, AffNormal, AffNormal, AffNormal, AffNormal, AffNormal},
          0xbf}, // elements 0..5 and 7 discovered
         {88,
          150,
          20,
          {0x31, 0x32},
          {AffWeak,    AffNormal, AffNull,   AffResist, AffFlagged, AffWeak,   AffNormalPct,
           AffWeakPct, AffResist, AffNull,   AffNormal, AffNormal,  AffNormal, AffNormal,
           AffNormal,  AffNormal, AffNormal, AffNormal, AffNormal,  AffNormal},
          0x3ff}}, // all discovered
        ElementOrder,
        {1, 3, 2, 4}};
    return f;
}
