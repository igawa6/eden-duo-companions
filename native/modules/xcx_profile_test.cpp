// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Party page profile (xcx_profile.h): the record and stat decoders on synthetic bytes always.
// Optional inputs (skipped when unset):
//   XCX_COMMON_BDAT + XCX_COMMON_MS_BDAT  the player's /bdat/common.bdat and /bdat/us/common_ms.bdat:
//                                         names and item columns of the fixture's gear (native captures 22/23)
//   XCX_SAVE_PAYLOAD                      a decoded gamedata payload (XCX-F01 gamedata01): the four
//                                         members' records at 0x48 + id * 0x7f8
#include "xcx_profile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
std::vector<std::uint8_t> File(const char *env) {
    const char *p = std::getenv(env);
    if (!p || !*p)
        return {};
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void Put16(std::vector<std::uint8_t> &b, std::size_t o, std::uint16_t v) { std::memcpy(b.data() + o, &v, 2); }
void Put32(std::vector<std::uint8_t> &b, std::size_t o, std::int32_t v) { std::memcpy(b.data() + o, &v, 4); }
} // namespace
int main() {
    using namespace xcx::profile;
    // A synthetic record shaped like Zanemato's (fixture values, PROFILE-DATA s2).
    std::vector<std::uint8_t> rec(RecordBytes, 0);
    std::memcpy(rec.data(), "Zanemato", 8);
    rec[0x7a] = 99;
    rec[0x2f0] = 1;
    Put16(rec, 0x2f4, 10);
    const auto slot = [&](int k, std::uint16_t item, std::uint16_t cat, std::uint16_t e0, std::uint16_t e1 = 0,
                          std::uint16_t e2 = 0) {
        const auto o = GearOffset + GearStride * k;
        Put16(rec, o, item);
        Put16(rec, o + 4, std::uint16_t(0x1240 | cat));
        Put16(rec, o + 8, e0);
        Put16(rec, o + 10, e1);
        Put16(rec, o + 12, e2);
    };
    slot(0, 1707, 6, 147);
    slot(1, 606, 7, 1329, 107, 2518);
    slot(2, 306, 2, 106);
    slot(4, 307, 3, 46);
    slot(5, 308, 4, 46);
    slot(6, 309, 5, 266);
    auto r = DecodeRecord(rec, "Zanemato");
    Check(r.valid && r.level == 99 && r.class_id == 1 && r.rank == 10, "record header");
    Check(r.gear[1].item == 606 && r.gear[1].effects[2] == 2518 && r.gear[3].item == 0, "gear slots");
    Check(!DecodeRecord(rec, "Elma").valid, "other name refused");
    Check(DecodeRecord(rec).valid, "no expected name");
    Check(!DecodeRecord(std::span(rec).first(0x200)).valid, "short refused");
    {
        auto bad = rec;
        Put16(bad, GearOffset + GearStride * 2 + 4, 0x1243); // torso slot holding an arm piece
        Check(!DecodeRecord(bad).valid, "wrong category refused");
        bad = rec;
        bad[0x7a] = 0;
        Check(!DecodeRecord(bad).valid, "level 0 refused");
        bad = rec;
        bad[0] = 0;
        Check(!DecodeRecord(bad).valid, "empty name refused");
    }
    // Block 2 (status + 0x250) with Zanemato's live values.
    std::vector<std::uint8_t> blk(StatsBytes, 0);
    Put32(blk, 0x20, 214);
    Put32(blk, 0x24, 80);
    Put32(blk, 0x28, 116);
    Put32(blk, 0x2c, 177);
    Put32(blk, 0x30, 410);
    Put32(blk, 0x34, 360);
    Put32(blk, 0x38, 180);
    Put32(blk, 0x40, 40);
    Put32(blk, 0x54, -60);
    auto s = DecodeStats(blk);
    Check(s.valid && s.melee == 214 && s.defense == 177 && s.evasion == 180 && s.res[0] == 40 && s.res[5] == -60,
          "stats");
    Put32(blk, 0x38, -1);
    Check(!DecodeStats(blk).valid, "negative evasion refused");
    Check(RankText(10, 10) == "MAX" && RankText(7, 20) == "7" && RankText(0, 10) == "--", "rank text");
    for (const auto &d : DisplaySlots)
        Check(d.record_slot >= 0 && d.record_slot < int(GearSlots), "display slot range");
    Check(DisplaySlots[0].label == "Melee" && DisplaySlots[0].record_slot == 1 && DisplaySlots[1].record_slot == 0,
          "native order");

    // The player's BDAT files: the fixture gear's names (native captures 22/23).
    const auto common = File("XCX_COMMON_BDAT"), ms = File("XCX_COMMON_MS_BDAT");
    Names names;
    if (!common.empty() && !ms.empty()) {
        Check(names.Load(common, ms), "names load");
        const auto knife = names.Weapon(606);
        Check(knife.name == "Carbide Knife II" && knife.level == 60 && knife.rarity == 5 && knife.attack == 63,
              "Carbide Knife II");
        const auto gun = names.Weapon(1707);
        Check(gun.name == "Delta Assailer" && gun.attack == 16, "Delta Assailer");
        Check(names.Armor(306).name == "Reginal Bodywear" && names.Armor(309).name == "Reginal Footwear",
              "armor names");
        Check(names.Effect(1329) == "Piscinoid Criticals Up VII" && names.Effect(107) == "Melee Attack Up VII" &&
                  names.Effect(2518) == "Arts: Recover HP VII",
              "effects");
        Check(names.Class(1) == "Drifter" && names.MaxRank(1) == 10, "Drifter");
        Check(names.Class(27) == "Full Metal Jaguar" && names.MaxRank(27) == 20, "Full Metal Jaguar");
        Check(names.Weapon(0).name.empty() && names.Weapon(60000).name.empty(), "out of range");
        std::printf("names: ok\n");
    } else
        std::printf("names: skipped (XCX_COMMON_BDAT / XCX_COMMON_MS_BDAT unset)\n");
    // The fixture save's four members.
    const auto save = File("XCX_SAVE_PAYLOAD");
    if (!save.empty()) {
        struct Want {
            int id;
            const char *name;
            int cls, rank;
            std::uint16_t melee, ranged;
        };
        for (const Want w : {Want{0, "Zanemato", 1, 10, 606, 1707}, Want{11, "Elma", 27, 20, 328, 1888},
                             Want{12, "Lin", 28, 20, 869, 2444}, Want{14, "Irina", 30, 20, 606, 1646}}) {
            const std::size_t o = 0x48 + std::size_t(w.id) * RecordStride;
            Check(o + RecordBytes <= save.size(), "save size");
            if (o + RecordBytes > save.size())
                continue;
            const auto m = DecodeRecord(std::span(save).subspan(o, RecordBytes), w.name);
            Check(m.valid && m.level == 99 && m.class_id == w.cls && m.rank == w.rank, w.name);
            Check(m.gear[1].item == w.melee && m.gear[0].item == w.ranged, "fixture weapons");
            if (names.Ready() && w.id == 0)
                for (const auto &d : DisplaySlots) {
                    const auto &g = m.gear[d.record_slot];
                    const auto text = !g.item ? std::string()
                                      : d.record_slot < 2 ? names.Weapon(g.item).name
                                                          : names.Armor(g.item).name;
                    std::printf("  %-10s %s\n", std::string(d.label).c_str(), text.c_str());
                }
        }
        std::printf("save: ok\n");
    } else
        std::printf("save: skipped (XCX_SAVE_PAYLOAD unset)\n");
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
