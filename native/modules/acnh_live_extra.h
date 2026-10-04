// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion, data lane: live values the main reader (acnh_live.h) does not sample, each from
// its own pinned code (acnh_live_extra.cpp XPin*, a mismatch closes only that part). Research
// notes: research/acnh/impl/fix/data.md.
//
//   Critterpedia  PlayerOther.ItemCollectBit (what the game's Critterpedia shows as caught).
//                 (The open Critterpedia page's tab / focused entry reader, page =
//                 [[[UI root]+0x90 home seq]+0x988]+0x12d0, tab page+0x38198, entry list+0x43c, was
//                 removed in round 8: nothing read it. research/acnh/impl/fix/data.md.)
//   Nook Miles+   BonusVDaily[512] (personal 0x12728) and player flag DailyQuestFivePointQuest:
//                 the x5 task = IsFivePoint 0x269f560 = BonusDaily && (BonusVDaily || flag == uid).
//   Visitors      the game's special-visitor list 0x24aa590 (after the day's VisitorNpc):
//                 Celeste 0x27d6104 (game-day weekday == Land.VisitorNpc+0x74, not online),
//                 Daisy Mae 0x27d4428 (Land.Shop.ShopLevel >= 2 on a calendar Sunday),
//                 Harvey 0x27d6250 (Land flags SpnVisitMainField != 0, TodayGlobalEventId ==
//                 0xffff, not online). K.K. (0x27d4550, round 7:
//                 research/acnh/impl/fix/r7-pages.md) needs the Land EventFlag array, the LandTemp
//                 flag array and each player's TotakekeLiveCount / BirthdayLiveDate: the reader
//                 collects them (kk_*), the publisher decides with the calendar table
//                 (Publisher::KkDay) and drops Celeste / Harvey on a K.K. day.
//   Map           Land.NpcHouseList (main.dat 0x481D20, schema) and the Land flags HotelBuilt /
//                 HotelConstruction for the map lane's icons (owner, level, hotel stage).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "acnh_live.h"

namespace acnh::live {

struct ExtraSnapshot {
    // ---- caught critters as the game's Critterpedia shows them (round 7) ----
    /// PlayerOther.ItemCollectBit (personal 0x49938, 0x754 bytes): bit (id & 7) of byte id >> 3 =
    /// item id ever collected. LIVE A/B (r7-pages.md §5): the Critterpedia shows a sea creature
    /// when its bit is set, whatever DiveFishCollection holds (list alone: not shown).
    bool collect_ok = false;
    std::vector<uint8_t> collect_bits;
    bool Collected(uint16_t id) const {
        return collect_ok && (id >> 3) < collect_bits.size() &&
               (collect_bits[id >> 3] >> (id & 7) & 1);
    }

    // ---- Nook Miles+ ----
    bool nmp_ok = false;
    std::array<uint8_t, 512> bonus_v{}; ///< BonusVDaily
    int five_quest = -1;                ///< DailyQuestFivePointQuest flag value (65535 = none)

    // ---- special visitors (in the game's list order) ----
    bool visitors_ok = false;
    std::vector<std::string> visitors; ///< NPC labels: "ows" Celeste, "boc" Daisy Mae, "spn" Harvey
    bool online = false;
    /// K.K. inputs: Land.EventFlag u16[640] (main.dat 0x22ED00, save handle +0xe75f0),
    /// Land.EventFlagTemp u16[128] (main.dat 0x5785A0, save handle +0x205208 = 0x2643000's node),
    /// and per island player (AccountTable 0x1E34C0 + 0x48 p, AccountUid != 0) the player flag
    /// TotakekeLiveCount (personal 0xC280 + 2 uid) and Player.BirthdayLiveDate (personal 0x36A1A:
    /// u16 y, u8 m, u8 d). kk_ok = all of them read (every used account's personal found).
    bool kk_ok = false;
    std::vector<uint16_t> kk_land, kk_temp;
    struct KkPlayer {
        int no = -1, count = -1, y = 0, m = 0, d = 0;
    };
    std::vector<KkPlayer> kk_players;

    // ---- map page: villager houses + hotel stage for the map icons (acnh_map.h IslandInput) ----
    bool houses_ok = false;
    std::vector<uint8_t> npc_houses; ///< main.dat Land.NpcHouseList.HouseList (0x481D20, 0xBD10)
    uint32_t houses_rev = 0;         ///< bumps when npc_houses changes (the map's cache key)
    int hotel_state = -1;            ///< 0 none, 1 HotelConstruction, 2 HotelBuilt (Land flags)
    /// renovation flags of the Map app's name rule (0x2a60460, acnh_map.h FacilityFlags): Land
    /// EventFlag values of OfficeConstruction1, MarketConstruction2, MuseumConstruction1/2/3 (-1
    /// not read)
    int office_construction1 = -1, market_construction2 = -1;
    int museum_construction[3] = {-1, -1, -1};
    /// map page residents (mapzoom lane): the island's players = Land.PlayerVillagerAccountTable
    /// entries with a non-zero AccountUid (main.dat 0x1e34c0 + 0x48 k), name from that entry's
    /// PlayerId, PastDaysFromMade (personal.dat 0x1352e) from the player's loaded personal data
    /// (-1 = not found). The Map app lists them oldest first (sort 0x256b370).
    struct MapPlayer {
        int no = -1;
        std::u16string name;
        int days = -1;
    };
    std::vector<MapPlayer> players;
    /// facility stages for the map name plates (0x2ff51a8): Land.Shop.ShopLevel,
    /// ShopTailor.TailorLevel, Museum.MuseumLevel, CampSite.CampSiteLevel (-1 = not read)
    int shop_level_map = -1, tailor_level = -1, museum_level = -1, camp_level = -1;
};

class ExtraReader {
public:
    struct Need {
        bool book = false, today = false, map = false;
    };
    /// `s` = this sample's main snapshot (personal address, phone state, game weekday).
    void Sample(const Guest& g, const LiveSnapshot& s, const Need& need, ExtraSnapshot& out);

    /// EventFlags*Param UniqueIDs the reader needs (looked up by the publisher from the tables).
    int flag_five_quest = -1;   ///< player DailyQuestFivePointQuest
    int land_spn_visit = -1;    ///< land SpnVisitMainField
    int land_global_event = -1; ///< land TodayGlobalEventId
    int land_hotel_built = -1;  ///< land HotelBuilt
    int land_hotel_work = -1;   ///< land HotelConstruction
    int flag_kk_count = -1;     ///< player TotakekeLiveCount
    int land_office_c1 = -1,
        land_market_c2 = -1;             ///< land OfficeConstruction1 / MarketConstruction2
    int land_museum_c[3] = {-1, -1, -1}; ///< land MuseumConstruction1/2/3

private:
    bool Resolve(const Guest& g);
    bool resolved = false;
    uint64_t main_base = 0, main_size = 0;
    uint32_t dead = 0; ///< parts whose pins did not match
    uint64_t houses_ms = 0, houses_source = 0;
    std::vector<uint8_t> houses_buf; ///< reused for each houses refresh

    uint64_t save_mgr = 0, session = 0;
};

} // namespace acnh::live
