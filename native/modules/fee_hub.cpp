// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_hub.h"
#include "fee_text.h"
#include "fee_guest.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "fee_pins.inc"

namespace fee_hub {
namespace pins = fee_pins;

namespace {
// ---- Local anchors (2026-10-30 pass 2) -----------------------------------------------------
// Resolved from script.json's ScriptMetadata (TypeInfo RVAs, main-relative) + dump.cs (field
// offsets), the same way contracts/STATE-FLAGS.md's rows were made; kept local to fee_hub.cpp
// (not fee_pins.inc) so three other agents can touch that shared file concurrently. Not
// live-verified beyond the synthetic test -- see fee_hub.h's header comment and the report for
// what a device/desktop live check still needs to confirm.
constexpr u64 TypeInfoChapterData = 0x6153a70;   // App.StructData<ChapterData>_TypeInfo
constexpr u64 TypeInfoGameTime = 0x6157a20;      // App.GameTime_TypeInfo (plain static fields)

constexpr u64 ChapterCid = 0x20, ChapterName = 0x28, ChapterNextChapter = 0x80;
constexpr u64 ChapterGmapSpotState = 0x90; // GmapSpot.State (i32): Active=3, CanSearch=8 publish
constexpr u64 ChapterFlag = 0xBC;          // ChapterData.Flags (i32 bitfield)
constexpr u64 ChapterRecommendedLevel = 0x108; // u8
constexpr u64 ChapterNation = 0x110, ChapterNationName = 0x118;
// ChapterData.m_PlaceName +0x148 ("MCID_M010_PLACE"): NationName is null on every row live and
// Nation is a Japanese NID_ with no message, so the world map shows the place line instead.
constexpr u64 ChapterPlaceName = 0x148;
// Somniel "Locations" (the native Somniel Map list): StructData<HubAreaData> AID +0x20, MID +0x28,
// ConditionCID +0x30, FacilityAidList +0x58 (string[]); StructData<HubFacilityData> AID +0x20,
// MID +0x28, ConditionCID +0x30, IconName +0x38 (minimap.bundle sprite).
constexpr u64 TypeInfoHubAreaData = 0x6153B30, TypeInfoHubFacilityData = 0x6153B40;
constexpr u64 AreaMid = 0x28, AreaCondition = 0x30, AreaFacilities = 0x58;
constexpr u64 FacilityAid = 0x20, FacilityMid = 0x28, FacilityCondition = 0x30, FacilityIcon = 0x38;
constexpr u64 TypeInfoGameUserData = 0x6152F38, GameUserDataChapter = 0x38, ChapterCidField = 0x20;
// GameUserData.m_MascotName (dump.cs: string at 0x90): the name the player gave the Somniel's
// mascot. Empty until named; the game's facility label is then MID_Hub_Mascot ("Shrine?").
constexpr u64 GameUserDataMascotName = 0x90;
constexpr s32 ChapterFlagSideStory = s32(1073741824); // 0x40000000: paralogue/skirmish bit
constexpr int GmapStateActive = 3, GmapStateCanSearch = 8;

constexpr u64 GameTimeStaticPlayTime = 0x18; // i32 ticks; seconds = ticks / 512 (GameTime.TimeInv)
constexpr int GameTimeTicksPerSecond = 512;

constexpr u64 HubAccessDataIsStory = 0x20, HubAccessDataIsReliance = 0x21,
              HubAccessDataIsGod = 0x22, HubAccessDataIsUnit = 0x23, HubAccessDataIsAnimal = 0x24,
              HubAccessDataIsPerson = 0x25; // all bool (1 byte)
// ---- Somniel map (2026-10-30, LIVE-READERS "Live verification 5") ---------------------------
constexpr u64 HubSeqSceneName = 0xA8;        // string ("Solanel")
constexpr u64 HubSeqPlayer = 0xD0;           // HubPlayerController
constexpr u64 HubSeqMiniMap = 0x130;         // HubMiniMap
constexpr u64 PlayerDir = 0x10;              // float, degrees (yaw)
constexpr u64 PlayerRoot = 0x28;             // GameObject (GetTransform() = its transform)
constexpr u64 MiniCenter = 0x110, MiniSize = 0x11C, MiniUv = 0x138, MiniOffset = 0x148,
              MiniIconList = 0x160, MiniInitialized = 0x28, MiniReverse = 0x128;
// HubMiniMap.m_image (App.ResourceHandle) -> m_Path: the map art the game itself loaded
// (dump.cs TypeDefIndex 10739 / 13958).
constexpr u64 MiniImage = 0x38, ResourceHandlePath = 0x10;
constexpr u64 IconAccess = 0x10, IconTalkL = 0x50, IconActive = 0x58;
constexpr u64 AccessAid = 0x18, AccessOrigPosition = 0x40, AccessData = 0x60;
constexpr u64 AccessDataDispos = 0x18, AccessDataMessages = 0x48;
constexpr u64 DisposContentType = 0x68, DisposScriptName = 0x90;
constexpr u64 UnityCachedPtr = 0x10;         // UnityEngine.Object.m_CachedPtr (0 = destroyed)
constexpr u64 ProcInstBind = 0x5C;           // ProcInst.IsBind (0x281E6E0): a child proc runs
// The game's own getters, called through the HUD call stub (read-only).
constexpr u64 RvaGameObjectGetTransform = 0x2C4E880; // GameObject.get_transform(this)
constexpr u64 RvaComponentGetTransform = 0x2C46440;  // Component.get_transform(this)
constexpr u64 RvaTransformGetPosition = 0x378F8F0;   // Transform.get_position_Injected(this, out)
constexpr u64 RvaAccessGetAccessCount = 0x2168320;   // HubAccess.get_AccessCount(this)
enum CallKind : u32 { KPlayerXform = 1, KPlayerPos, KPersonXform, KPersonPos, KTalked, KTickets, KNewest, KTimezone };
constexpr int MaxIcons = 64;
constexpr char MiniMapBundle[] = "fe_assets_ui/hub/minimap/textures/minimap.bundle";
constexpr char MapImageDir[] = "fe_assets_ui/hub/minimap/mapimage/";
// Marker sizes in map-image pixels (the image is shown at about 0.7x on the 740 px map area):
// people ~45 px and facility tiles ~36 px on screen, as D2 09 (Thor recheck 2026-11-01: people
// were ~20 px, hard to tell apart).
// FE3H-style GPS page (2026-11-01): the 1200x1350 picture is shown at 0.59x when zoomed out and
// ~1.4x at the default player view, so markers scale with zoom like the FE3H monastery map.
constexpr float PeoplePx = 54.f, FacilityPx = 40.f, DotPx = 22.f, PlayerPx = 44.f, TalkPx = 32.f;
constexpr char TalkBundle[] = "fe_assets_ui/hub/accesshelp/textures/talkicon.bundle";
constexpr char IconTableBundle[] = "fe_assets_gamedata/hubmapicon.xml.bundle";
constexpr int MaxChapterRows = 512;
u64 Fnv(u64 h, const void* p, std::size_t n);  // StructData<ChapterData>.s_list size (the whole game's table)
} // namespace

Hub::Hub(const EdenDsmodHostApi& host) : host_{host}, base_{host.main_base}, size_{host.main_size} {}

bool Hub::Read(u64 at, void* out, std::size_t n) const {
    return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host_, at, out, n);
}

u64 Hub::StaticPtr(u64 type_info, u64 field) const {
    const u64 klass = Ptr(Slot(type_info)).value_or(0);
    if (!klass)
        return 0;
    const u64 statics = Ptr(klass + pins::KlassStatics).value_or(0);
    if (!statics)
        return 0;
    return Ptr(statics + field).value_or(0);
}

std::optional<std::string> Hub::ManagedString(u64 at, std::size_t max_chars) const {
    const auto w = fee_guest::ManagedChars(
        [this](u64 a, void* out, std::size_t n) { return Read(a, out, n); }, at, max_chars);
    if (!w)
        return std::nullopt;
    return fee_guest::AsciiKey(*w);
}

bool Hub::Resolve(const EdenDsmodHostApi& host) {
    if (resolved_)
        return true;
    base_ = host.main_base;
    size_ = host.main_size;
    if (!base_ || !size_)
        return false;
    if (host.get_i64)
        delta_ = host.get_i64(host.userdata, "__relocation_delta", 0);
    // Sanity, same anchor fee_reader uses: UnitPool's TypeInfo slot must resolve to a mapped
    // klass whose statics are mapped too.
    const u64 klass = Ptr(Slot(pins::TypeInfoUnitPool)).value_or(0);
    if (!klass || !host.is_mapped || !host.is_mapped(host.userdata, klass, pins::KlassStatics + 8))
        return false;
    const u64 statics = Ptr(klass + pins::KlassStatics).value_or(0);
    if (!statics || !host.is_mapped(host.userdata, statics, 8))
        return false;
    resolved_ = true;
    return true;
}

int Hub::SceneMode() const {
    if (StaticPtr(pins::TypeInfoHubSequence, 0))
        return 1; // Somniel
    if (StaticPtr(pins::TypeInfoGmapSequence, 0))
        return 2; // world map
    return 3; // battle / title / loading / unresolved
}

bool Hub::ReadRow(u64 at, RosterRow& row) const {
    if (!at)
        return false;
    row.at = at;
    const auto lv = Get<u8>(at + pins::UnitLevel);
    const auto hp = Get<u8>(at + pins::UnitHp);
    if (!lv || !hp)
        return false;
    row.lv = *lv;
    row.hp = *hp;
    const u64 person = Ptr(at + pins::UnitPerson).value_or(0);
    if (person) {
        row.pid = ManagedString(Ptr(person + pins::PersonPid).value_or(0)).value_or("");
        row.name = ManagedString(Ptr(person + pins::PersonName).value_or(0)).value_or("");
        row.fid = ManagedString(Ptr(person + pins::PersonFid).value_or(0)).value_or("");
        row.icon = ManagedString(Ptr(person + pins::PersonIcon).value_or(0)).value_or("");
    }
    if (const u64 edit = Ptr(at + pins::UnitEdit).value_or(0))
        if (const s32 g = Get<s32>(edit + pins::UnitEditGender).value_or(0); g != 0)
            row.icon = fee_text::GenderedIcon(row.icon, g);
    const u64 job = Ptr(at + pins::UnitJob).value_or(0);
    if (job)
        row.job = ManagedString(Ptr(job + pins::JobName).value_or(0)).value_or("");
    const u64 god_unit = Ptr(at + pins::UnitGodUnit).value_or(0);
    row.bond = -1;
    row.god.clear();
    if (god_unit) {
        const u64 god = Ptr(god_unit + pins::GodUnitGod).value_or(0);
        if (god)
            row.god = ManagedString(Ptr(god + pins::GodDataName).value_or(0)).value_or("");
        const u64 holder = Ptr(god_unit + pins::GodUnitBonds).value_or(0);
        const u64 dict = holder ? Ptr(holder + pins::BondHolderDict).value_or(0) : 0;
        const u64 entries = dict ? Ptr(dict + pins::DictEntries).value_or(0) : 0;
        const auto n = entries ? Get<s32>(entries + pins::ArrayLength) : std::nullopt;
        if (entries && n && *n > 0 && !row.pid.empty()) {
            for (int i = 0; i < *n; ++i) {
                const u64 e = entries + pins::ArrayData + u64(i) * pins::DictEntrySize;
                const u64 key = Ptr(e + pins::DictEntryKey).value_or(0);
                if (!key)
                    continue;
                const auto k = ManagedString(key);
                if (k && *k == row.pid) {
                    const u64 bond = Ptr(e + pins::DictEntryValue).value_or(0);
                    if (bond) {
                        const auto lvl = Get<u8>(bond + pins::GodBondLevel);
                        if (lvl)
                            row.bond = *lvl;
                    }
                    break;
                }
            }
        }
    }
    return true;
}

bool Hub::ReadRoster() {
    roster_.clear();
    pid_dot_.clear();
    pid_row_.clear();
    // Force[0] == Force.Type.Player (the owner's army). Walk m_Head via Unit.m_Next, capped.
    const u64 forces = StaticPtr(pins::TypeInfoUnitPool, pins::UnitPoolForces);
    if (!forces)
        return false;
    const u64 force0 = Ptr(forces + pins::ArrayData).value_or(0);
    if (!force0)
        return false;
    // Force 0 (Player) holds the army only inside a battle; in Somniel / on the world map the
    // whole army sits in Force 3 (Absent) -- live Ch.26 Somniel: Force 0 empty, Force 3 41 units.
    for (const int f : {0, 3}) {
        const u64 force = f == 0 ? force0 : Ptr(forces + pins::ArrayData + 8u * u64(f)).value_or(0);
        u64 at = force ? Ptr(force + pins::ForceHead).value_or(0) : 0;
        for (int guard = 0; at && guard < 120 && int(roster_.size()) < 120; ++guard) {
            RosterRow row;
            if (ReadRow(at, row) && !row.pid.empty()) {
                if (extra_)
                    extra_(at, row);
                roster_.push_back(row);
            }
            at = Ptr(at + pins::UnitNext).value_or(0);
        }
    }
    pid_row_.clear();
    for (std::size_t i = 0; i < roster_.size(); ++i) {
        const RosterRow& r = roster_[i];
        if (!r.pid8.empty() && !r.dot.empty())
            pid_dot_[r.pid8] = r.dot;
        if (!r.pid8.empty())
            pid_row_.emplace(r.pid8, i);
    }
    return true;
}

std::optional<std::string> Hub::ManagedUtf8(u64 at, std::size_t max_chars) const {
    const auto w = fee_guest::ManagedChars(
        [this](u64 a, void* out, std::size_t n) { return Read(a, out, n); }, at, max_chars);
    if (!w)
        return std::nullopt;
    return fee_guest::Utf16ToUtf8(*w);
}

std::string Hub::RuleLine(const std::string& mid) const {
    if (mid.empty() || !text_)
        return {};
    const std::string raw = text_(mid);
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\x01' || i + 2 >= raw.size()) {
            out += raw[i] == '\n' || raw[i] == '\r' ? ' ' : raw[i];
            continue;
        }
        const char g = raw[i + 1], t = raw[i + 2];
        i += 2;
        if (g == '6' && t == '3') // the lord (Alear) by name, as MapSituation rule strings
            out += Localized("MPID_Lueur");
    }
    return out;
}

bool Hub::ReadChapter(u64 at, ChapterRow& row) const {
    if (!at)
        return false;
    const auto state = Get<s32>(at + ChapterGmapSpotState);
    if (!state)
        return false;
    row.state = *state;
    row.lv = int(Get<u8>(at + ChapterRecommendedLevel).value_or(0xFF));
    const auto flag = Get<s32>(at + ChapterFlag).value_or(0);
    row.side_story = (u32(flag) & u32(ChapterFlagSideStory)) != 0;
    row.cid = ManagedString(Ptr(at + ChapterCid).value_or(0)).value_or("");
    row.name = ManagedString(Ptr(at + ChapterName).value_or(0)).value_or("");
    row.next = ManagedString(Ptr(at + ChapterNextChapter).value_or(0)).value_or("");
    row.nation = ManagedString(Ptr(at + ChapterNation).value_or(0)).value_or("");
    row.nation_name = ManagedString(Ptr(at + ChapterNationName).value_or(0)).value_or("");
    row.place = ManagedString(Ptr(at + ChapterPlaceName).value_or(0)).value_or("");
    return !row.cid.empty();
}

void Hub::ReadChapters() {
    chapters_.clear();
    // StructData<ChapterData>.s_list: a List<ChapterData*> (same generic List<T> layout the
    // SkillData/TerrainData readers in fee_reader.cpp already use via ListItems/ListSize).
    const u64 list = StaticPtr(TypeInfoChapterData, 0);
    if (!list)
        return;
    const u64 items = Ptr(list + pins::ListItems).value_or(0);
    const auto size = Get<s32>(list + pins::ListSize);
    if (!items || !size || *size <= 0 || *size > MaxChapterRows)
        return;
    for (int i = 0; i < *size; ++i) {
        const u64 data = Ptr(items + pins::ArrayData + std::size_t(i) * 8).value_or(0);
        ChapterRow row;
        if (ReadChapter(data, row) &&
            (row.state == GmapStateActive || row.state == GmapStateCanSearch))
            chapters_.push_back(std::move(row));
    }
    // The chapter the save is on (GameUserData.m_Chapter): what the native world map points
    // "To Next Chapter" at (live Ch.26 save: CID_M026 "Fallen Somniel", while the first
    // Active/CanSearch non-SideStory row was the finished Fell Xenologue 1).
    current_ = {};
    // Preferred: the game's own "next chapter" spot (GetNewestSpot, live: Slot 1 after Ch.9
    // points at Ch.10 while GameUserData.m_Chapter still names the cleared Ch.9).
    u64 spot = 0;
    {
        std::scoped_lock lock{map_mutex_};
        spot = newest_spot_;
    }
    if (spot) {
        const u64 list = Ptr(spot + pins::GmapSpotChapters).value_or(0);
        const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
        if (items && Get<s32>(list + pins::ListSize).value_or(0) > 0)
            ReadChapter(Ptr(items + pins::ArrayData).value_or(0), current_);
    }
    if (current_.cid.empty())
        if (const u64 gud = StaticPtr(TypeInfoGameUserData, 0))
            if (const u64 chap = Ptr(gud + GameUserDataChapter).value_or(0))
                ReadChapter(chap, current_);
}

void Hub::Publish(const EdenDsmodHostApi& host) {
    const auto i64 = [&](const char* name, s64 v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    };
    const auto text = [&](const char* name, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, v.c_str());
    };
    i64("hub.ready", resolved_ && mode_ == 1 ? 1 : 0);
    i64("gmap.ready", resolved_ && mode_ == 2 ? 1 : 0);
    i64("roster.filter", filter_);
    // Somniel menu header: Money (GameUserData.m_Gold), Bond Fragments (m_PieceOfBond), Relay
    // Tickets (RelayUserGlobalData.GetTicketCount through the call stub).
    {
        const u64 gud = (mode_ == 1 || mode_ == 2) ? StaticPtr(TypeInfoGameUserData, 0) : 0;
        const auto gold = gud ? Get<s32>(gud + pins::GameUserDataGold) : std::nullopt;
        const auto bond = gud ? Get<s32>(gud + pins::GameUserDataPieceOfBond) : std::nullopt;
        const bool gok = gold && *gold >= 0 && *gold <= 9999999;
        const bool bok = bond && *bond >= 0 && *bond <= 9999999;
        i64("hub.hasgold", gok ? 1 : 0);
        int tickets = -1;
        {
            std::scoped_lock lock{map_mutex_};
            tickets = (mode_ == 1 || mode_ == 2) ? tickets_ : -1;
        }
        // One header line like the native menu: "Relay Tickets: 8  Bond Fragments 9,999,999
        // Money 1,076,499 G" (labels in the label colour).
        const auto grouped = [](s64 v) {
            std::string d = std::to_string(v), out;
            for (std::size_t i = 0; i < d.size(); ++i) {
                if (i && (d.size() - i) % 3 == 0)
                    out += ',';
                out += d[i];
            }
            return out;
        };
        // Two lines (the Map tab's 500 px header could not hold all three: Money was cut off).
        std::string l1, l2;
        const std::string lab = "{c:#FFD4E2F3}", end = "{/c}";
        if (tickets >= 0)
            l1 += lab + "Relay Tickets:" + end + " " + std::to_string(tickets);
        if (bok)
            l1 += (l1.empty() ? "" : "\n") + lab + "Bond Fragments" + end + " " + grouped(*bond);
        if (gok)
            l2 += lab + "Money" + end + " " + grouped(*gold) + " G";
        text("hub.money.line", l1 + (l1.empty() || l2.empty() ? "" : "    ") + l2);
        text("hub.money.l1", l1);
        text("hub.money.l2", l2);
    }
    // World map skirmishes: GmapSpotManager spots with a placed GmapMobUnit (encounter).
    {
        int n = 0;
        std::string places;
        const u64 mgr = mode_ == 2 ? StaticPtr(pins::TypeInfoGmapSpotManager, 0) : 0;
        const u64 arr = mgr ? Ptr(mgr + pins::GmapSpotArray).value_or(0) : 0;
        const u32 len = arr ? Get<u32>(arr + pins::ArrayLength).value_or(0) : 0;
        for (u32 i = 0; i < len && i < 256 && n < 8; ++i) {
            const u64 spot = Ptr(arr + pins::ArrayData + 8 * u64(i)).value_or(0);
            if (!spot || !Ptr(spot + pins::GmapSpotMobUnit).value_or(0))
                continue;
            const u64 list = Ptr(spot + pins::GmapSpotChapters).value_or(0);
            const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
            const u64 chap = items && Get<s32>(list + pins::ListSize).value_or(0) > 0
                                 ? Ptr(items + pins::ArrayData).value_or(0)
                                 : 0;
            ChapterRow row;
            if (chap)
                ReadChapter(chap, row);
            const std::string place = row.place.empty() ? std::string{} : Localized(row.place);
            if (!place.empty())
                places += (places.empty() ? "" : ", ") + place;
            ++n;
        }
        i64("gmap.sk.n", n);
        text("gmap.sk.line", n ? "{c:#FFD4E2F3}Skirmishes{/c} " + std::to_string(n) +
                                     (places.empty() ? std::string{} : "  " + places)
                               : std::string{});
    }

    // ---- Somniel: facility/people inventory (names + kind only -- see fee_hub.h for the
    // position/heading/minimap gap) and play time ("Current Time" hero, see fee_hub.h).
    {
        // Somniel map image: a content key per distinct frame (positions rounded to a pixel).
        std::scoped_lock lock{map_mutex_};
        const bool has = mode_ == 1 && map_.ok && unity_;
        i64("hub.hasmap", has ? 1 : 0);
        i64("hub.mapwhy", mode_ == 1 ? map_why_ : -1);
        i64("hub.maptex", tex_failed_ ? 0 : 1);
        if (has) {
            GlideMarkers(std::chrono::steady_clock::now());
            u64 hsh = 0xCBF29CE484222325ull;
            hsh = Fnv(hsh, map_.image.data(), map_.image.size());
            const auto q = [&](float f) {
                const s32 v = s32(std::lround(f * 4));
                hsh = Fnv(hsh, &v, 4);
            };
            q(map_.cx), q(map_.cz), q(map_.sx), q(map_.sz), q(map_.ux), q(map_.uy), q(map_.uw),
                q(map_.uh), q(map_.ox), q(map_.oz), q(map_.reverse ? 1.f : 0.f);
            if (map_.player)
                q(map_.px), q(map_.pz), q(std::round(map_.dir / 5) * 5);
            map_.sel = sel_person_;
            hsh = Fnv(hsh, &map_.sel, sizeof map_.sel);
            for (const HubIcon& ic : map_.icons) {
                const u8 flags = u8((ic.active ? 1 : 0) | (ic.talk ? 2 : 0) | (ic.talked == 1 ? 4 : 0));
                q(ic.x), q(ic.z);
                hsh = Fnv(hsh, &flags, 1);
                hsh = Fnv(hsh, ic.icon.data(), ic.icon.size());
                hsh = Fnv(hsh, ic.dot.data(), ic.dot.size());
            }
            char key[48];
            std::snprintf(key, sizeof key, "fee:hub/map/%016llx", static_cast<unsigned long long>(hsh));
            if (map_key_ != key) {
                map_key_ = key;
                if (!frames_.count(map_key_)) {
                    frames_[map_key_] = map_;
                    frame_order_.push_back(map_key_);
                }
                while (frame_order_.size() > 10) {
                    // Never drop the shown or the prefetched frame.
                    const std::string old = frame_order_.front();
                    frame_order_.erase(frame_order_.begin());
                    if (old == pipe_.shown || old == pipe_.pending)
                        frame_order_.push_back(old);
                    else
                        frames_.erase(old);
                }
            }
            const bool rendered_pending =
                !pipe_.pending.empty() &&
                std::find(rendered_.begin(), rendered_.end(), pipe_.pending) != rendered_.end();
            const auto cur = frames_.find(pipe_.shown);
            pipe_.Step(map_key_, rendered_pending, std::chrono::steady_clock::now(),
                       cur == frames_.end() || cur->second.image != map_.image);
            text("hub.mapkey", "module:" + pipe_.shown);
            text("hub.mapnext", "module:" + (pipe_.pending.empty() ? pipe_.shown : pipe_.pending));
        } else {
            pipe_.Clear();
        }
    }
    PublishSomniel(host);
    {
        // Somniel "Locations" (native Somniel Map list): localized area names + facility icons.
        const int pn = mode_ == 1 ? std::min<int>(int(places_.size()), 16) : 0;
        i64("hub.loc.n", pn);
        for (int i = 0; i < pn; ++i) {
            const Place& pl = places_[std::size_t(i)];
            char name[40];
            std::snprintf(name, sizeof name, "hub.loc%d.name", i);
            text(name, pl.name);
            const int icn = std::min<int>(int(pl.icons.size()), 4);
            std::snprintf(name, sizeof name, "hub.loc%d.icn", i);
            i64(name, icn);
            for (int k = 0; k < icn; ++k) {
                std::snprintf(name, sizeof name, "hub.loc%d.ic%d", i, k);
                text(name, "module:fee:hub/icon/" + pl.icons[std::size_t(k)]);
            }
        }
    }
    {
        int tod = -1;
        {
            std::scoped_lock lock{map_mutex_};
            tod = mode_ == 1 ? tod_ : -1;
        }
        static constexpr const char* TodKeys[4] = {"MID_Hub_Time_Morning", "MID_Hub_Time_Noon",
                                                   "MID_Hub_Time_Evening", "MID_Hub_Time_Night"};
        i64("hub.hastod", tod >= 0 ? 1 : 0);
        text("hub.tod.name", tod >= 0 ? Localized(TodKeys[tod]) : std::string{});
    }
    i64("hub.hastime", 0);
    if (mode_ == 1) {
        const u64 statics = [&] {
            const u64 klass = Ptr(Slot(TypeInfoGameTime)).value_or(0);
            return klass ? Ptr(klass + pins::KlassStatics).value_or(0) : u64{0};
        }();
        if (const auto play_ticks = statics ? Get<s32>(statics + GameTimeStaticPlayTime) : std::nullopt) {
            const int total_seconds = std::max(0, *play_ticks) / GameTimeTicksPerSecond;
            i64("hub.time.h", total_seconds / 3600);
            i64("hub.time.m", (total_seconds / 60) % 60);
            i64("hub.hastime", 1);
        }
    }

    // ---- World map: current chapter + available paralogues/skirmishes. Gated entirely by
    // GmapSpotState (Active/CanSearch only -- see fee_hub.h), the game's own spoiler boundary.
    const ChapterRow* current = current_.cid.empty() ? nullptr : &current_;
    std::vector<const ChapterRow*> side;
    for (const auto& c : chapters_) {
        if (c.side_story)
            side.push_back(&c);
        else if (!current)
            current = &c; // fallback: first non-SideStory Active/CanSearch row this sample
    }
    i64("gmap.hasdata", mode_ == 2 && current ? 1 : 0);
    if (mode_ == 2 && current) {
        // "Chapter 10  The Fell Dragon Sombron" in the player's words; place under it.
        const std::string pre = Localized(current->name + "_PREFIX");
        const std::string name = Localized(current->name);
        text("gmap.chapter", pre.empty() ? name : name.empty() ? pre : pre + "  " + name);
        text("gmap.chapter.nation", Localized(current->place));
        // Chapter thumbnail (gmap/infothumb, "CID_M010" -> "M010") and the chapter's own rule
        // lines: MID_RULE_<M010>_WIN / _LOSE (else MID_RULE_COMMON_LOSE), the texts the map
        // script hands MapSituation when the battle starts.
        const std::string tail = current->cid.starts_with("CID_") ? current->cid.substr(4) : current->cid;
        text("gmap.chapter.thumb", tail.empty() ? std::string{}
                                                : "module:fee:sprite/infothumb/" + tail + "/440x240");
        std::string win = RuleLine("MID_RULE_" + tail + "_WIN");
        std::string lose = RuleLine("MID_RULE_" + tail + "_LOSE");
        if (lose.empty())
            lose = RuleLine("MID_RULE_COMMON_LOSE");
        text("gmap.chapter.win", win);
        text("gmap.chapter.lose", lose);
        i64("gmap.chapter.hasrule", win.empty() ? 0 : 1);
    }
    const int gn = mode_ == 2 ? std::min<int>(int(side.size()), 16) : 0;
    i64("gmap.n", gn);
    for (int i = 0; i < gn; ++i) {
        const auto& c = *side[std::size_t(i)];
        char name[32];
        const auto set_t = [&](const char* suffix, const std::string& v) {
            std::snprintf(name, sizeof(name), "gmap.g%d.%s", i, suffix);
            text(name, v);
        };
        const auto set_i = [&](const char* suffix, s64 v) {
            std::snprintf(name, sizeof(name), "gmap.g%d.%s", i, suffix);
            i64(name, v);
        };
        {
            const std::string pre = Localized(c.name + "_PREFIX");
            const std::string name = Localized(c.name);
            set_t("name", pre.empty() ? name : name.empty() ? pre : pre + "  " + name);
        }
        set_t("nation", Localized(c.place));
        set_i("state", c.state); // 3 Active (sortie-ready), 8 CanSearch
        if (c.lv != 0xFF)
            set_i("lv", c.lv);
    }

    // ---- Roster: the army force, filtered (roster_filter action), capped and republished
    // contiguously so the manifest's repeat_bind sees a dense 0..n-1 range.
    std::vector<const RosterRow*> filtered;
    for (const auto& r : roster_) {
        if (filter_ >= FilterSword && filter_ <= FilterBow && r.weapon != filter_)
            continue;
        if (filter_ == FilterNoRing && !r.god.empty())
            continue;
        filtered.push_back(&r);
    }
    sel_unit_ = 0;
    if (mode_ == 1 || mode_ == 2) {
        const int n = std::min<int>(int(filtered.size()), MaxRosterRows);
        i64("roster.n", n);
        i64("roster.sel", sel_);
        for (int i = 0; i < n; ++i) {
            const auto& r = *filtered[std::size_t(i)];
            char name[32];
            const auto set_i = [&](const char* suffix, s64 v) {
                std::snprintf(name, sizeof(name), "roster.r%d.%s", i, suffix);
                i64(name, v);
            };
            const auto set_t = [&](const char* suffix, const std::string& v) {
                std::snprintf(name, sizeof(name), "roster.r%d.%s", i, suffix);
                text(name, v);
            };
            set_i("lv", r.lv);
            set_i("hp", r.hp);
            const std::string nm = Localized(r.name);
            set_t("name", nm.empty() ? Localized(r.job) : nm); // unnamed: the class, as the game
            set_t("job", Localized(r.job));
            set_i("mhp", r.mhp);
            set_t("face", r.face);
            set_i("hasface", r.face.empty() ? 0 : 1);
            set_t("ring", r.ring);
            set_i("hasring", r.ring.empty() ? 0 : 1);
            set_t("engimg", r.engimg);
        }
        if (sel_ >= 0 && sel_ < n) {
            const auto& r = *filtered[std::size_t(sel_)];
            sel_unit_ = r.at;
            i64("roster.s.lv", r.lv);
            i64("roster.s.hp", r.hp);
        }
    } else {
        i64("roster.n", 0);
        i64("roster.sel", -1);
    }
}

void Hub::Sample(const EdenDsmodHostApi& host) {
    host_ = host;
    base_ = host.main_base;
    size_ = host.main_size;
    if (!Resolve(host)) {
        mode_ = 0;
        roster_.clear();
        Publish(host);
        return;
    }
    mode_ = SceneMode();
    if (mode_ == 1 || mode_ == 2)
        ReadRoster();
    else
        roster_.clear();
    ReadMap(); // clears itself outside Somniel
    if (mode_ == 1)
        ReadPlaces();
    else
        places_.clear();
    if (mode_ == 2)
        ReadChapters();
    else
        chapters_.clear();
    if (sel_ >= int(roster_.size()))
        sel_ = -1;
    Publish(host);
}

bool Hub::OnAction(const char* action, std::int64_t argument) {
    if (!action)
        return false;
    std::string_view a{action};
    if (a == "roster_pick") {
        sel_ = int(argument);
        if (sel_ < -1)
            sel_ = -1;
        return true;
    }
    if (a == "hub_person" || a == "hub_prow" || a == "hub_pca") {
        // People tap: hub_person <person index>, hub_prow <Map tab row>, hub_pca <People
        // tab row>. Selects and centres the map on them; the same
        // person again (or none) clears back to the player.
        s64 pi = argument;
        if (a == "hub_prow")
            pi = argument >= 0 && argument < s64(pr_person_.size()) ? pr_person_[std::size_t(argument)] : -1;
        else if (a != "hub_person")
            pi = argument >= 0 && argument < s64(pc_person_.size())
                     ? pc_person_[std::size_t(argument)].first
                     : -1;
        const u64 at = pi >= 0 && pi < s64(person_order_.size()) ? person_order_[std::size_t(pi)] : 0;
        sel_person_ = (at && at != sel_person_) ? at : 0;
        ++view_reset_;
        return true;
    }
    if (a == "roster_filter") {
        filter_ = int(argument);
        if (filter_ < FilterAll || filter_ > FilterNoRing)
            filter_ = FilterAll;
        sel_ = -1; // the selection index is only valid within one filtered list
        return true;
    }
    return false;
}

} // namespace fee_hub


// ---- Somniel map ---------------------------------------------------------------------------
namespace fee_hub {
namespace {
/// HubMiniMap.GetDisposName (0x28BAEF0): the hubmapicon.xml row an access point uses.
std::string DisposName(u32 content_type, const std::string& script, bool unit_god_story,
                       bool has_messages) {
    if (content_type == 6 || content_type == 8 || content_type == 9)
        return "ItemPoint";
    if (content_type == 0)
        return "MovePoint";
    if (!script.empty())
        return script;
    if (unit_god_story)
        return "Unit";
    if (content_type == 7)
        return "AnimalPoint";
    return has_messages ? "Mob1" : "Mob2";
}

u64 Fnv(u64 h, const void* p, std::size_t n) {
    return dsmod_sdk::Fnv1a64(static_cast<const u8*>(p), n, h);
}

void Blend(std::vector<u8>& dst, u32 dw, u32 dh, int x, int y, const u8* s, float alpha) {
    if (x < 0 || y < 0 || u32(x) >= dw || u32(y) >= dh)
        return;
    u8* d = &dst[(std::size_t(y) * dw + u32(x)) * 4];
    const float a = s[3] / 255.f * alpha;
    for (int c = 0; c < 3; ++c)
        d[c] = u8(std::lround(d[c] * (1 - a) + s[c] * a));
    d[3] = u8(std::min(255.f, d[3] + s[3] * alpha * (1 - d[3] / 255.f)));
}

/// Draws `src` (sub-rect sx0,sy0,sw,sh) centred at (cx,cy), scaled to `size` px on its long
/// side, rotated by `deg` clockwise (bilinear-free nearest sampling: icons are small).
void Stamp(std::vector<u8>& dst, u32 dw, u32 dh, const lp_unity::Image& src, int sx0, int sy0,
           int sw, int sh, float cx, float cy, float size, float deg, float alpha) {
    // The sub-rect must lie inside the source (a romfs sprite can differ per game patch/DLC).
    if (sw <= 0 || sh <= 0 || sx0 < 0 || sy0 < 0 || u64(sx0) + u64(sw) > src.width ||
        u64(sy0) + u64(sh) > src.height || src.rgba.size() < std::size_t(src.width) * src.height * 4)
        return;
    const float scale = size / float(std::max(sw, sh));
    const float hw = sw * scale / 2, hh = sh * scale / 2;
    const float r = std::sqrt(hw * hw + hh * hh) + 1;
    const float rad = deg * 3.14159265f / 180.f, cs = std::cos(rad), sn = std::sin(rad);
    for (int y = int(cy - r); y <= int(cy + r); ++y)
        for (int x = int(cx - r); x <= int(cx + r); ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            // inverse rotation into the sprite frame
            const float lx = dx * cs + dy * sn, ly = -dx * sn + dy * cs;
            const int u = int(std::floor((lx + hw) / scale)), v = int(std::floor((ly + hh) / scale));
            if (u < 0 || v < 0 || u >= sw || v >= sh)
                continue;
            const u8* p = &src.rgba[(std::size_t(sy0 + v) * src.width + u32(sx0 + u)) * 4];
            if (p[3])
                Blend(dst, dw, dh, x, y, p, alpha);
        }
}
} // namespace

void Hub::LogLine(int level, const std::string& msg) const {
    if (host_.log)
        host_.log(host_.userdata, level, msg.c_str());
}

std::string MapImageName(std::string_view path, std::string_view scene) {
    // Preferred: the game's own ResourceHandle path, ".../MapImage/M002" -> "M002".
    std::string_view n = path;
    if (const auto slash = n.find_last_of('/'); slash != std::string_view::npos)
        n.remove_prefix(slash + 1);
    if (const auto dot = n.find('.'); dot != std::string_view::npos)
        n = n.substr(0, dot);
    if (!n.empty())
        return std::string{n};
    // Fallback: the scene name. "Hub_Solanel" -> "Solanel" (live 2026-10-30), "Fld_M002" -> "M002"
    // (post-battle exploration; the bundles are mapimage/m002.bundle, texture "M002").
    std::string s{scene};
    for (const char* pre : {"hub_", "fld_"}) {
        if (s.size() > 4 && std::equal(pre, pre + 4, s.begin(), [](char a, char b) {
                return a == char(std::tolower(static_cast<unsigned char>(b)));
            })) {
            s.erase(0, 4);
            break;
        }
    }
    return s;
}

void Hub::ReadMap() {
    HubMap m;
    const u64 seq = StaticPtr(pins::TypeInfoHubSequence, 0);
    const u64 mini = seq ? Ptr(seq + HubSeqMiniMap).value_or(0) : 0;
    // 0 ok, 1 no HubSequence, 2 no HubMiniMap, 3 minimap object dead, 4 minimap not initialized,
    // 5 minimap parameters unreadable/degenerate.
    int why = !seq ? 1 : !mini ? 2 : !Ptr(mini + UnityCachedPtr).value_or(0) ? 3
              : !Get<u8>(mini + MiniInitialized).value_or(0)               ? 4
                                                                            : 5;
    if (mini && Ptr(mini + UnityCachedPtr).value_or(0) && Get<u8>(mini + MiniInitialized).value_or(0)) {
        std::array<float, 3> c{}, sz{};
        std::array<float, 4> uv{};
        std::array<float, 2> off{};
        if (Read(mini + MiniCenter, c.data(), 12) && Read(mini + MiniSize, sz.data(), 12) &&
            Read(mini + MiniUv, uv.data(), 16) && Read(mini + MiniOffset, off.data(), 8) &&
            sz[0] > 1 && sz[2] > 1 && uv[2] > 0 && uv[3] > 0) {
            m.ok = true;
            why = 0;
            m.cx = c[0], m.cz = c[2], m.sx = sz[0], m.sz = sz[2];
            m.ux = uv[0], m.uy = uv[1], m.uw = uv[2], m.uh = uv[3];
            m.ox = off[0], m.oz = off[1];
            m.reverse = Get<u8>(mini + MiniReverse).value_or(0) != 0;
            m.sprite = MapImageName(
                ManagedString(Ptr(Ptr(mini + MiniImage).value_or(0) + ResourceHandlePath).value_or(0), 128)
                    .value_or(""),
                ManagedString(Ptr(seq + HubSeqSceneName).value_or(0), 40).value_or(""));
            m.image = m.sprite;
            for (char& ch : m.image)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    u64 root = 0;
    float dir = 0;
    if (m.ok) {
        const u64 player = Ptr(seq + HubSeqPlayer).value_or(0);
        root = player ? Ptr(player + PlayerRoot).value_or(0) : 0;
        dir = player ? Get<float>(player + PlayerDir).value_or(0.f) : 0.f;
        const u64 list = Ptr(mini + MiniIconList).value_or(0);
        const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
        const int n = list ? Get<s32>(list + pins::ListSize).value_or(0) : 0;
        for (int i = 0; items && i < n && i < MaxIcons; ++i) {
            const u64 icon = Ptr(items + pins::ArrayData + std::size_t(i) * 8).value_or(0);
            const u64 access = icon ? Ptr(icon + IconAccess).value_or(0) : 0;
            if (!access || !Ptr(access + UnityCachedPtr).value_or(0))
                continue;
            const u64 data = Ptr(access + AccessData).value_or(0);
            const u64 dispos = data ? Ptr(data + AccessDataDispos).value_or(0) : 0;
            if (!data || !dispos)
                continue;
            HubIcon h;
            h.access = access;
            h.aid = ManagedString(Ptr(access + AccessAid).value_or(0), 64).value_or("");
            // People are army units keyed by their Pid (hubdispos AID "PID_..."): draw their own
            // map sprite instead of a plain dot.
            if (const auto pid = ManagedUtf8(Ptr(access + AccessAid).value_or(0))) {
                h.pid = *pid;
                if (const auto d = pid_dot_.find(*pid); d != pid_dot_.end())
                    h.dot = d->second;
            }
            if (const u64 str = Ptr(access + AccessAid).value_or(0)) {
                const auto len = Get<s32>(str + pins::StringLength);
                if (len && *len > 0 && *len <= 64) {
                    h.aid16.resize(std::size_t(*len));
                    if (!Read(str + pins::StringData, h.aid16.data(), h.aid16.size() * 2))
                        h.aid16.clear();
                    while (!h.aid16.empty() && h.aid16.back() == u'\0')
                        h.aid16.pop_back();
                }
            }
            std::array<float, 3> p{};
            if (!Read(access + AccessOrigPosition, p.data(), 12))
                continue;
            h.x = p[0], h.z = p[2];
            std::array<u8, 6> f{};
            Read(data + HubAccessDataIsStory, f.data(), f.size());
            h.reliance = f[1] != 0;
            h.person = f[5] != 0;
            const u64 msgs = Ptr(data + AccessDataMessages).value_or(0);
            const bool has_msgs = msgs && Get<s32>(msgs + pins::ListSize).value_or(0) > 0;
            h.icon = DisposName(Get<u32>(dispos + DisposContentType).value_or(0),
                                ManagedString(Ptr(dispos + DisposScriptName).value_or(0), 40).value_or(""),
                                f[3] || f[2] || f[0], has_msgs);
            h.active = Get<u8>(icon + IconActive).value_or(0) != 0;
            h.talk = Ptr(icon + IconTalkL).value_or(0) != 0;
            m.icons.push_back(std::move(h));
        }
    }

    if (mode_ == 1 && why != map_why_) {
        static constexpr const char* Why[] = {"ok", "no HubSequence", "no HubMiniMap", "HubMiniMap object dead",
                                              "HubMiniMap not initialized", "HubMiniMap parameters unreadable"};
        LogLine(why ? EDEN_DSMOD_LOG_WARNING : EDEN_DSMOD_LOG_INFO,
                std::string{"FE Engage DSMod: Somniel map "} + (why ? "not available: " : "") + Why[why] +
                    (why ? "" : " (image " + m.image + ")"));
        map_why_ = why;
    } else if (mode_ != 1) {
        map_why_ = -1;
    }
    std::scoped_lock lock{map_mutex_};
    if (!m.ok) {
        map_ = {};
        player_root_ = 0;
        xform_.clear();
        pos_.clear();
        talked_.clear();
        jobs_.clear();
        job_next_ = 0;
        return;
    }
    player_root_ = root;
    m.dir = dir;
    if (const auto t = xform_.find(root); root && t != xform_.end())
        if (const auto p = pos_.find(t->second); p != pos_.end())
            m.player = true, m.px = p->second[0], m.pz = p->second[2];
    for (HubIcon& h : m.icons) {
        if (h.person)
            if (const auto t = xform_.find(h.access); t != xform_.end())
                if (const auto p = pos_.find(t->second); p != pos_.end())
                    h.x = p->second[0], h.z = p->second[2];
        if (const auto c = talked_.find(h.access); c != talked_.end())
            h.talked = c->second > 0 ? 1 : 0;
    }
    if (job_next_ >= jobs_.size()) {
        // One refresh round: player first, then the people the native map follows, then the
        // talk counters. Transforms are looked up once per object.
        jobs_.clear();
        job_next_ = 0;
        if (root && xform_.find(root) == xform_.end())
            jobs_.push_back({KPlayerXform, root}); // its position: every other call (NextCall)
        for (const HubIcon& h : m.icons) {
            if (h.person && h.active) {
                const auto t = xform_.find(h.access);
                if (t == xform_.end())
                    jobs_.push_back({KPersonXform, h.access});
                else
                    jobs_.push_back({KPersonPos, t->second});
            }
            if (h.talk && !h.reliance)
                jobs_.push_back({KTalked, h.access});
        }
    }
    map_ = std::move(m);
}

void Hub::GlideMarkers(std::chrono::steady_clock::time_point now) {
    // The player and people positions arrive a few times a second (one game call per frame,
    // shared by everyone); drawn as read they jump. Each marker glides linearly from where it is
    // drawn to the newest read over the measured read interval, so a walk is a smooth motion.
    // At rest the glide ends on the read value exactly (same frame key, no renders).
    if (map_.player)
        glide_[0].Step(map_.px, map_.pz, now, map_.px, map_.pz);
    else
        glide_.erase(0);
    for (HubIcon& ic : map_.icons)
        if (ic.person && ic.access)
            glide_[ic.access].Step(ic.x, ic.z, now, ic.x, ic.z);
    if (glide_.size() > 256)
        glide_.clear();
}

void Glide::Step(float rx, float rz, std::chrono::steady_clock::time_point now, float& x, float& z) {
    using namespace std::chrono;
    if (!had || std::hypot(rx - tx, rz - tz) > Teleport) {
        had = true;
        fx = tx = x = rx, fz = tz = z = rz;
        since = now;
        return;
    }
    const auto at = [&](time_point<steady_clock> t, float& ox, float& oz) {
        const double a = std::clamp(duration<double, std::milli>(t - since).count() / std::max(ms, 1.0), 0.0, 1.0);
        if (a >= 1.0)
            ox = tx, oz = tz; // exactly the read value: a marker at rest stops changing
        else
            ox = float(fx + (tx - fx) * a), oz = float(fz + (tz - fz) * a);
    };
    if (rx != tx || rz != tz) {
        // A new read: glide from where the marker is drawn now to it, over the read interval.
        const double gap = duration<double, std::milli>(now - since).count();
        ms = std::clamp(ms * 0.6 + gap * 0.4, MinMs, MaxMs);
        at(now, fx, fz);
        tx = rx, tz = rz;
        since = now;
    }
    at(now, x, z);
}

void FramePipe::Step(const std::string& newest, bool pending_rendered,
                     std::chrono::steady_clock::time_point now, bool restart) {
    if (newest.empty()) {
        Clear();
        return;
    }
    if (shown.empty() || restart) {
        shown = newest;
        pending.clear();
        seen = false;
        return;
    }
    if (pending_rendered) {
        if (seen) {
            shown = pending;
            pending.clear();
            seen = false;
            ++swaps;
        } else {
            seen = true;
        }
    }
    if (newest == shown) {
        pending.clear();
        seen = false;
    } else if (pending != newest &&
               (pending.empty() || (!seen && now - since > std::chrono::milliseconds(250)))) {
        pending = newest;
        since = now;
        seen = false;
    }
}

void ViewFollow::Step(double tx, double ty, bool is_centred, int reset) {
    const bool snap = !had || rs != reset || centred != is_centred || std::abs(tx - cx) > Jump ||
                      std::abs(ty - cy) > Jump;
    if (snap) {
        cx = tx, cy = ty;
    } else {
        cx += (tx - cx) * Ease;
        cy += (ty - cy) * Ease;
        if (std::abs(tx - cx) < Settle)
            cx = tx;
        if (std::abs(ty - cy) < Settle)
            cy = ty;
    }
    had = true;
    rs = reset;
    centred = is_centred;
}

bool Hub::UnitCallsSafe() const {
    // Somniel: HubSequence runs free. World map: GmapSequence (same ProcSceneSequence base, so
    // the same ProcInst.IsBind) runs free -- no spot menu, army menu, chapter entry or scene
    // change bound under it. Unit.GetSimpleHit only reads the unit's own data (no Unity object).
    if (mode_ != 1 && mode_ != 2)
        return false;
    const u64 seq = StaticPtr(mode_ == 1 ? pins::TypeInfoHubSequence : pins::TypeInfoGmapSequence, 0);
    return seq && Get<s32>(seq + ProcInstBind).value_or(1) == 0;
}

std::optional<HubCall> Hub::NextCall(u64 scratch) {
    std::scoped_lock lock{map_mutex_};
    // Relay Tickets (Somniel header): RelayUserGlobalData.GetTicketCount(), once a second while
    // in the Somniel or on the world map.
    if (mode_ == 1 || mode_ == 2) {
        if (++ticket_gap_ >= 60 && !inflight_) {
            if (const u64 relay = StaticPtr(pins::TypeInfoRelayUserGlobalData, 0)) {
                ticket_gap_ = 0;
                inflight_ = relay;
                return HubCall{pins::RelayGetTicketCount, {relay, 0, 0, 0}, CallTagBase + KTickets};
            }
        }
    } else {
        ticket_gap_ = 60;
        tickets_ = -1;
    }
    // Somniel "Current Time" (Morning/Afternoon/Evening/Night): HubUtil.GetTimezoneType()
    // (static, reads the game's own time-of-day variable), every two seconds in the Somniel.
    if (mode_ == 1) {
        if (++tod_gap_ >= 120 && !inflight_) {
            tod_gap_ = 0;
            inflight_ = 1; // static call: no object
            return HubCall{pins::HubGetTimezoneType, {0, 0, 0, 0}, CallTagBase + KTimezone};
        }
    } else {
        tod_gap_ = 120;
        tod_ = -1;
    }
    // World map "To Next Chapter": GmapSpotManager.GetNewestSpot() (the spot the native L button
    // points at), every two seconds on the world map.
    if (mode_ == 2) {
        if (++newest_gap_ >= 120 && !inflight_) {
            if (const u64 mgr = StaticPtr(pins::TypeInfoGmapSpotManager, 0)) {
                newest_gap_ = 0;
                inflight_ = mgr;
                return HubCall{pins::GmapGetNewestSpot, {mgr, 0, 0, 0}, CallTagBase + KNewest};
            }
        }
    } else {
        newest_gap_ = 120;
        newest_spot_ = 0;
    }
    if (!map_.ok || !scratch || mode_ != 1)
        return std::nullopt;
    // Only while the Somniel sequence runs free (ProcInst.IsBind == 0: no menu, fast travel or
    // scene change in progress). Objects die during a departure; a getter on one throws an
    // uncaught C++ exception (live 2026-10-30: crash when leaving for the world map).
    const u64 seq = StaticPtr(pins::TypeInfoHubSequence, 0);
    if (!seq || Get<s32>(seq + ProcInstBind).value_or(1) != 0) {
        call_gap_ = 0;
        return std::nullopt;
    }
    // One hub call per frame at most (the HUD stub serves one per frame anyway); every other one
    // reads the player's position, so the arrow follows a walk at ~30 Hz instead of once per
    // round of every person and talk counter (2026-11-01, owner: "map movement is step by step").
    player_turn_ = !player_turn_;
    if (player_turn_ && player_root_) {
        if (const auto t = xform_.find(player_root_); t != xform_.end() && t->second &&
                                                       Ptr(t->second + UnityCachedPtr).value_or(0)) {
            call_gap_ = 0;
            inflight_ = t->second;
            return HubCall{RvaTransformGetPosition, {t->second, scratch, 0, 0}, CallTagBase + KPlayerPos};
        }
    }
    while (job_next_ < jobs_.size()) {
        const auto [kind, obj] = jobs_[job_next_++];
        // Never hand the game a destroyed Unity object (its getter would throw).
        if (!obj || !Ptr(obj + UnityCachedPtr).value_or(0))
            continue;
        call_gap_ = 0;
        inflight_ = obj;
        const u32 tag = CallTagBase + kind;
        switch (kind) {
        case KPlayerXform:
            return HubCall{RvaGameObjectGetTransform, {obj, 0, 0, 0}, tag};
        case KPersonXform:
            return HubCall{RvaComponentGetTransform, {obj, 0, 0, 0}, tag};
        case KPlayerPos:
        case KPersonPos:
            return HubCall{RvaTransformGetPosition, {obj, scratch, 0, 0}, tag};
        case KTalked:
            return HubCall{RvaAccessGetAccessCount, {obj, 0, 0, 0}, tag};
        default:
            break;
        }
    }
    return std::nullopt;
}

void Hub::OnCallLost() {
    std::scoped_lock lock{map_mutex_};
    inflight_ = 0;
}

void Hub::OnCall(u32 tag, u64 x0, const u8* scratch, std::size_t n) {
    std::scoped_lock lock{map_mutex_};
    const u64 obj = inflight_;
    inflight_ = 0;
    if (!obj || tag < CallTagBase)
        return;
    switch (tag - CallTagBase) {
    case KPlayerXform:
    case KPersonXform:
        if (x0)
            xform_[obj] = x0;
        break;
    case KPlayerPos:
    case KPersonPos:
        if (scratch && n >= 12) {
            std::array<float, 3> p{};
            std::memcpy(p.data(), scratch, 12);
            if (std::isfinite(p[0]) && std::isfinite(p[2])) {
                pos_[obj] = p;
            }
        }
        break;
    case KTalked:
        talked_[obj] = int(s32(x0 & 0xFFFFFFFFu));
        break;
    case KTimezone: {
        const s32 t = s32(x0 & 0xFFFFFFFFu);
        tod_ = t >= 0 && t < 4 ? t : -1;
        break;
    }
    case KNewest:
        newest_spot_ = x0;
        break;
    case KTickets: {
        const u32 n = u32(x0 & 0xFFFFFFFFu);
        tickets_ = n <= 999 ? int(n) : -1; // RelayUserGlobalData.MaxTicketCount
        break;
    }
    default:
        break;
    }
}

std::string Hub::IconSprite(const std::string& dispos) {
    // Icon table (hubmapicon.xml: DisposName -> IconName), read once on the asset worker (Render);
    // the sample thread only looks names up (empty until the first map picture rendered).
    std::scoped_lock lock{table_mutex_};
    if (icon_table_.empty() && unity_ && dispos.empty()) {
        for (const auto& [name, id] : unity_->ListObjects(IconTableBundle, 49)) {
            const auto v = unity_->ReadObject(IconTableBundle, id);
            const auto* script = v ? v->Get("m_Script") : nullptr;
            const std::string xml{script ? script->AsString() : std::string_view{}};
            for (std::size_t at = xml.find("DisposName=\""); at != std::string::npos;
                 at = xml.find("DisposName=\"", at + 1)) {
                const std::size_t a = at + 12, b = xml.find('"', a);
                const std::size_t c = xml.find("IconName=\"", b), d = xml.find('"', c + 10);
                if (b == std::string::npos || c == std::string::npos || d == std::string::npos)
                    break;
                icon_table_[xml.substr(a, b - a)] = xml.substr(c + 10, d - c - 10);
            }
            break;
        }
        if (icon_table_.empty())
            icon_table_["?"] = "?"; // tried; do not re-read every frame
    }
    if (dispos.empty())
        return {};
    const auto it = icon_table_.find(dispos);
    return it != icon_table_.end() ? it->second : std::string{};
}

bool Hub::PicturePoint(const HubMap& m, const MapGeo& g, float wx, float wz, float& px, float& py) {
    float u = 0, v = 0;
    if (!g.ok || !MapPoint(m, wx, wz, u, v))
        return false;
    float x = (u * g.tex_w - g.crop_x0) * g.scale + g.offx;
    float y = (v * g.tex_h - g.crop_y0) * g.scale + g.offy;
    if (g.reverse)
        x = MapW - x, y = MapH - y;
    px = x / MapW;
    py = y / MapH;
    return std::isfinite(px) && std::isfinite(py);
}

void Hub::PublishSomniel(const EdenDsmodHostApi& host) {
    // FE3H Monastery structure in Engage style (owner 2026-11-01): sub-tabs Map / People / Legend.
    // Published: GPS view (hub.view.*), the player's place (hub.area), people grouped by place
    // (Map tab rows hub.pr{i}.*, People tab paired cards hub.pc{j}.*), the selected person
    // (hub.sel, hub.s.*), legend kinds present (hub.lm{k}.* markers, hub.lf{k}.* facilities).
    const auto i64 = [&](const std::string& n, s64 v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, n.c_str(), v);
    };
    const auto f64 = [&](const std::string& n, double v) {
        if (host.publish_f64)
            host.publish_f64(host.userdata, n.c_str(), v);
        else if (host.publish_i64)
            host.publish_i64(host.userdata, n.c_str(), s64(std::lround(v)));
    };
    const auto text = [&](const std::string& n, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, n.c_str(), v.c_str());
    };
    HubMap m;
    MapGeo g;
    HubMap shown; // the frame on screen (hub.mapkey): the camera centres on what is drawn
    {
        std::scoped_lock lock{map_mutex_};
        if (mode_ == 1 && map_.ok)
            m = map_;
        if (mode_ == 1)
            if (const auto it = frames_.find(pipe_.shown); it != frames_.end())
                shown = it->second;
        g = geo_;
        if (g.image != m.image)
            g.ok = false;
    }
    const bool live = mode_ == 1 && m.ok;
    // Facility markers on the map with their place (HubAccess.AID == HubFacilityData.AID; the
    // marker's own icon name as a fallback join).
    struct Spot {
        float x, z;
        int area;
    };
    std::vector<Spot> spots;
    std::vector<std::pair<std::string, std::string>> fac_legend; // icon sprite, name
    std::vector<std::pair<std::string, std::string>> mark_legend;
    bool have_unit = false, have_talk = false, have_mob = false, have_animal = false, have_item = false,
         have_move = false;
    std::string mob_spr = "Mob1", animal_spr = "Animal", item_spr = "Item"; // drawn sprite of each kind
    std::vector<const HubIcon*> people;
    if (live) {
        for (const HubIcon& ic : m.icons) {
            if (!ic.active)
                continue;
            const std::string spr = IconSprite(ic.icon);
            const Facility* fa = nullptr;
            for (const Facility& f : facilities_)
                if ((!ic.aid16.empty() && f.aid == ic.aid16) || (!spr.empty() && f.icon == spr)) {
                    fa = &f;
                    break;
                }
            if (fa) {
                spots.push_back({ic.x, ic.z, fa->area});
                bool seen = false;
                for (const auto& [i, n] : fac_legend)
                    seen |= i == fa->icon;
                if (!seen && !fa->name.empty() && !fa->icon.empty())
                    fac_legend.emplace_back(fa->icon, fa->name);
            }
            if (ic.talk)
                have_talk = true;
            if (ic.person && pid_row_.count(ic.pid) && pid_row_.at(ic.pid) < roster_.size()) {
                people.push_back(&ic);
                have_unit = true;
                continue;
            }
            if (ic.icon == "Unit")
                have_unit = true;
            else if (ic.icon == "Mob1" || ic.icon == "Mob2")
                have_mob = true, mob_spr = spr.empty() ? mob_spr : spr;
            else if (ic.icon == "AnimalPoint")
                have_animal = true, animal_spr = spr.empty() ? animal_spr : spr;
            else if (ic.icon == "ItemPoint")
                have_item = true, item_spr = spr.empty() ? item_spr : spr;
            else if (spr == "MovePoint")
                have_move = true;
        }
    }
    const auto area_at = [&](float x, float z) {
        int best = -1;
        float bd = 1e30f;
        for (const Spot& s : spots) {
            const float d = (s.x - x) * (s.x - x) + (s.z - z) * (s.z - z);
            if (d < bd)
                bd = d, best = s.area;
        }
        return best;
    };
    const int here = live && m.player ? area_at(m.px, m.pz) : -1;
    const auto area_name = [&](int a) {
        return a >= 0 && a < int(places_.size()) ? places_[std::size_t(a)].name : std::string{};
    };
    i64("hub.hasarea", here >= 0 ? 1 : 0);
    text("hub.area", here >= 0 ? area_name(here) : std::string{});

    // People grouped by place: the player's place first, then the game's place order; people
    // with no place known go last under no header name ("Somniel" in the manifest).
    struct Person {
        const HubIcon* ic;
        int area;
        const RosterRow* row;
    };
    std::vector<Person> ps;
    for (const HubIcon* ic : people)
        ps.push_back({ic, area_at(ic->x, ic->z), &roster_[pid_row_.at(ic->pid)]});
    const auto rank = [&](int a) { return a == here ? -2 : a < 0 ? 1000 : a; };
    std::stable_sort(ps.begin(), ps.end(), [&](const Person& a, const Person& b) {
        return rank(a.area) < rank(b.area);
    });
    if (ps.size() > 40)
        ps.resize(40);
    person_order_.clear();
    pr_person_.clear();
    pc_person_.clear();
    for (const Person& p : ps)
        person_order_.push_back(p.ic->access);
    bool sel_ok = false;
    for (const Person& p : ps)
        sel_ok |= p.ic->access == sel_person_;
    if (!sel_ok)
        sel_person_ = 0;
    const auto pname = [&](const Person& p) {
        const std::string n = Localized(p.row->name);
        return n.empty() ? Localized(p.row->job) : n;
    };
    // Map tab rows: header (kind 0) / person (kind 1).
    int r = 0, c = 0;
    std::size_t i = 0;
    while (i < ps.size() && r < 60) {
        const int a = ps[i].area;
        std::size_t j = i;
        while (j < ps.size() && ps[j].area == a)
            ++j;
        const std::string hn = "hub.pr" + std::to_string(r) + ".";
        pr_person_.push_back(-1);
        i64(hn + "kind", 0);
        text(hn + "name", area_name(a));
        i64(hn + "here", a == here && a >= 0 ? 1 : 0);
        i64(hn + "n", s64(j - i));
        ++r;
        // People tab: header, then one row per person.
        const std::string hc = "hub.pc" + std::to_string(c) + ".";
        pc_person_.push_back({-1, -1});
        i64(hc + "kind", 0);
        text(hc + "name", area_name(a));
        i64(hc + "here", a == here && a >= 0 ? 1 : 0);
        i64(hc + "n", s64(j - i));
        ++c;
        for (std::size_t k = i; k < j && r < 60; ++k) {
            const Person& p = ps[k];
            const std::string pn = "hub.pr" + std::to_string(r) + ".";
            pr_person_.push_back(int(k));
            i64(pn + "kind", 1);
            text(pn + "name", pname(p));
            text(pn + "face", p.row->rface);
            i64(pn + "hasface", p.row->rface.empty() ? 0 : 1);
            i64(pn + "talk", p.ic->talk && !(p.ic->talked == 1 && !p.ic->reliance) ? 1 : 0);
            i64(pn + "pi", s64(k));
            i64(pn + "sel", p.ic->access == sel_person_ ? 1 : 0);
            ++r;
        }
        // People tab: one person per row, full pane width (owner, Thor 2026-11-01: one column).
        for (std::size_t k = i; k < j && c < 60; ++k) {
            const std::string pc = "hub.pc" + std::to_string(c) + ".";
            pc_person_.push_back({int(k), -1});
            i64(pc + "kind", 1);
            i64(pc + "nb", 1);
            const Person& p = ps[k];
            text(pc + "aname", pname(p));
            text(pc + "aface", p.row->rface);
            i64(pc + "ahasface", p.row->rface.empty() ? 0 : 1);
            i64(pc + "atalk", p.ic->talk && !(p.ic->talked == 1 && !p.ic->reliance) ? 1 : 0);
            i64(pc + "api", s64(k));
            i64(pc + "asel", p.ic->access == sel_person_ ? 1 : 0);
            ++c;
        }
        i = j;
    }
    i64("hub.pr.n", r);
    i64("hub.pc.n", c);
    i64("hub.people.n", s64(ps.size()));

    // Selected person card + the GPS view: the player by default, the selected person while one
    // is picked (a pick bumps hub.view.rs, so even a user-moved view snaps to them).
    const Person* sel = nullptr;
    for (const Person& p : ps)
        if (p.ic->access == sel_person_)
            sel = &p;
    i64("hub.sel", sel ? s64(sel - ps.data()) : -1);
    if (sel) {
        text("hub.s.name", pname(*sel));
        text("hub.s.face", sel->row->rface);
        i64("hub.s.hasface", sel->row->rface.empty() ? 0 : 1);
        text("hub.s.area", area_name(sel->area));
        i64("hub.s.talk", sel->ic->talk && !(sel->ic->talked == 1 && !sel->ic->reliance) ? 1 : 0);
    }
    float cx = 0.5f, cy = 0.5f;
    bool centred = false;
    if (live && g.ok) {
        // Positions of the frame on screen, so the marker sits where the camera points.
        const HubMap& f = shown.ok && shown.image == m.image ? shown : m;
        if (sel)
            for (const HubIcon& ic : f.icons)
                if (ic.access == sel->ic->access)
                    centred = PicturePoint(f, g, ic.x, ic.z, cx, cy);
        if (!centred && f.player)
            centred = PicturePoint(f, g, f.px, f.pz, cx, cy);
    }
    cx = std::clamp(cx, 0.f, 1.f), cy = std::clamp(cy, 0.f, 1.f);
    view_.Step(cx, cy, centred, view_reset_);
    f64("hub.view.cx", view_.cx);
    f64("hub.view.cy", view_.cy);
    f64("hub.view.z", centred ? 2.4 : 1.0);
    i64("hub.view.rs", view_reset_);

    // Legend: only the kinds on the map now (FE3H owner rule).
    if (live && m.player)
        mark_legend.emplace_back("module:fee:hub/icon/Lueur", "You");
    if (have_unit)
        mark_legend.emplace_back("module:fee:hub/icon/Unit", "Allies");
    if (have_talk)
        mark_legend.emplace_back("module:fee:hub/talk", "Wants to talk");
    if (have_mob)
        mark_legend.emplace_back("module:fee:hub/icon/" + mob_spr, "Townsfolk");
    if (have_animal)
        mark_legend.emplace_back("module:fee:hub/icon/" + animal_spr, "Animals");
    if (have_item)
        mark_legend.emplace_back("module:fee:hub/icon/" + item_spr, "Items");
    if (have_move)
        mark_legend.emplace_back("module:fee:hub/icon/MovePoint", "Fast travel");
    i64("hub.lm.n", s64(mark_legend.size()));
    for (std::size_t k = 0; k < mark_legend.size(); ++k) {
        text("hub.lm" + std::to_string(k) + ".img", mark_legend[k].first);
        text("hub.lm" + std::to_string(k) + ".name", mark_legend[k].second);
    }
    const std::size_t nf = std::min<std::size_t>(fac_legend.size(), 20);
    i64("hub.lf.n", s64(nf));
    for (std::size_t k = 0; k < nf; ++k) {
        text("hub.lf" + std::to_string(k) + ".img", "module:fee:hub/icon/" + fac_legend[k].first);
        text("hub.lf" + std::to_string(k) + ".name", fac_legend[k].second);
    }
    i64("hub.lmrows", s64((mark_legend.size() + 1) / 2));
}

bool Hub::MapPoint(const HubMap& m, float wx, float wz, float& u, float& v) {
    // HubMiniMap.TransformPosition (0x28BCC00): local = (world - center - offset) /
    // (size * uv.size) * sizeDelta on a RawImage showing uv; as a texture coordinate that is
    // uv.center + (world - center - offset) / size. Unity textures are bottom-up, our images
    // top-down.
    if (!m.ok || m.sx == 0 || m.sz == 0)
        return false;
    u = m.ux + m.uw / 2 + (wx - m.cx - m.ox) / m.sx;
    const float tv = m.uy + m.uh / 2 + (wz - m.cz - m.oz) / m.sz;
    v = 1.f - tv;
    return std::isfinite(u) && std::isfinite(v);
}

std::shared_ptr<const lp_unity::Image> Hub::Sprite(const std::string& bundle,
                                                   const std::string& name) {
    const std::string key = bundle + "|" + name;
    {
        std::scoped_lock lock{sprite_mutex_};
        if (const auto it = sprites_.find(key); it != sprites_.end())
            return it->second;
    }
    if (!unity_)
        return nullptr;
    std::optional<lp_unity::Image> img = unity_->LoadSprite(bundle, name);
    if (!img)
        img = unity_->LoadTexture(bundle, name);
    auto ptr = img ? std::make_shared<const lp_unity::Image>(std::move(*img)) : nullptr;
    std::scoped_lock lock{sprite_mutex_};
    if (sprites_.size() > 48)
        sprites_.clear();
    sprites_[key] = ptr;
    return ptr;
}

bool Hub::Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h) {
    if (key == "fee:hub/talk") {
        // The native talk bubble (TalkIcon right half, as stamped on the map).
        const auto t = Sprite(TalkBundle, "TalkIcon");
        if (!t || t->width < 128 || t->height < 64)
            return false;
        w = h = 64;
        rgba.assign(std::size_t(w) * h * 4, 0);
        for (u32 y = 0; y < 64; ++y)
            std::memcpy(&rgba[std::size_t(y) * 64 * 4], &t->rgba[(std::size_t(y) * t->width + 64) * 4], 64 * 4);
        return true;
    }
    if (key.starts_with("fee:hub/icon/")) {
        // A Somniel facility icon (minimap.bundle sprite named by HubFacilityData.IconName).
        const auto spr = Sprite(MiniMapBundle, std::string{key.substr(13)});
        if (!spr || spr->rgba.empty())
            return false;
        w = spr->width;
        h = spr->height;
        rgba = spr->rgba;
        return true;
    }
    const auto render_t0 = std::chrono::steady_clock::now();
    HubMap m;
    {
        std::scoped_lock lock{map_mutex_};
        const auto it = frames_.find(std::string{key});
        if (it == frames_.end())
            return false;
        m = it->second;
    }
    if (!unity_ || m.image.empty())
        return false;
    IconSprite(""); // loads the hubmapicon.xml table once (asset worker thread)
    // Base: the map texture, cropped to its drawn content (alpha), fitted into MapW x MapH.
    // When it does not decode (logged once with the reason), the markers are still drawn on a
    // plain backdrop in the same frame of reference (a 2048 x 2048 texture, uncropped).
    if (!map_tex_ || base_name_ != m.image) {
        std::string sprite = m.sprite.empty() ? m.image : m.sprite;
        // The whole Texture2D: the Sprite of the same name is a tight crop of it, and the
        // TransformPosition texture coordinates are relative to the full texture (live 2026-10-30).
        std::shared_ptr<const lp_unity::Image> img;
        const std::string bundle = std::string{MapImageDir} + m.image + ".bundle";
        const auto t0 = std::chrono::steady_clock::now();
        if (auto t = unity_->LoadTexture(bundle, sprite))
            img = std::make_shared<const lp_unity::Image>(std::move(*t));
        const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count());
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        if (img && !img->rgba.empty()) {
            x0 = int(img->width), y0 = int(img->height);
            for (u32 y = 0; y < img->height; ++y)
                for (u32 x = 0; x < img->width; ++x)
                    if (img->rgba[(std::size_t(y) * img->width + x) * 4 + 3] > 24)
                        x0 = std::min(x0, int(x)), x1 = std::max(x1, int(x)),
                        y0 = std::min(y0, int(y)), y1 = std::max(y1, int(y));
        }
        if (x1 < x0) {
            if (!tex_logged_)
                LogLine(EDEN_DSMOD_LOG_ERROR,
                        "FE Engage DSMod: Somniel map image " + bundle + " (" + sprite + ") " +
                            (!img ? "failed to decode" : img->rgba.empty() ? "decoded empty" : "is fully transparent") +
                            " after " + std::to_string(ms) + " ms; drawing markers on a plain backdrop");
            tex_logged_ = true;
            tex_failed_ = true;
            auto blank = std::make_shared<lp_unity::Image>();
            blank->width = blank->height = 2048;
            img = blank;
            x0 = y0 = 0;
            x1 = y1 = 2047;
        } else {
            if (!tex_logged_)
                LogLine(EDEN_DSMOD_LOG_INFO, "FE Engage DSMod: Somniel map image " + sprite + " " +
                                                 std::to_string(img->width) + "x" + std::to_string(img->height) +
                                                 " decoded in " + std::to_string(ms) + " ms");
            tex_logged_ = true;
            tex_failed_ = false;
        }
        const int pad = 48;
        crop_x0_ = std::max(0, x0 - pad), crop_y0_ = std::max(0, y0 - pad);
        crop_x1_ = std::min(int(img->width) - 1, x1 + pad);
        crop_y1_ = std::min(int(img->height) - 1, y1 + pad);
        map_tex_ = img;
        base_name_ = m.image;
        base_for_.clear(); // a new texture: redraw the static layer
    }
    const auto& b = *map_tex_;
    const float cw = float(crop_x1_ - crop_x0_ + 1), ch = float(crop_y1_ - crop_y0_ + 1);
    const float scale = std::min(MapW / cw, MapH / ch);
    const float offx = (MapW - cw * scale) / 2, offy = (MapH - ch * scale) / 2;
    w = MapW;
    h = MapH;
    // m_reverse: the native map shows the texture turned 180 degrees (markers move with it, icons
    // stay upright, the player arrow turns with the map).
    const bool rev = m.reverse;
    // The static layer (the resampled texture) is drawn once per map image / orientation and
    // copied for every frame; only the markers are stamped per frame (2026-11-01: a walk minted
    // a new frame per position step and each one resampled the whole 1200x1350 texture).
    if (base_rgba_.empty() || base_for_ != m.image || base_rev_ != rev) {
        base_rgba_.assign(std::size_t(w) * h * 4, 0);
        if (b.rgba.empty()) {
            for (std::size_t i = 0; i < base_rgba_.size(); i += 4)
                base_rgba_[i] = 0x1E, base_rgba_[i + 1] = 0x26, base_rgba_[i + 2] = 0x34, base_rgba_[i + 3] = 0xFF;
        } else {
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    const u32 qx = rev ? w - 1 - x : x, qy = rev ? h - 1 - y : y;
                    const float sxf = (qx + 0.5f - offx) / scale + crop_x0_,
                                syf = (qy + 0.5f - offy) / scale + crop_y0_;
                    if (sxf < crop_x0_ || syf < crop_y0_ || sxf > crop_x1_ || syf > crop_y1_)
                        continue;
                    const u8* p = &b.rgba[(std::size_t(syf) * b.width + u32(sxf)) * 4];
                    std::memcpy(&base_rgba_[(std::size_t(y) * w + x) * 4], p, 4);
                }
        }
        base_for_ = m.image;
        base_rev_ = rev;
        LogLine(EDEN_DSMOD_LOG_INFO,
                "FE Engage DSMod: Somniel static map layer drawn in " +
                    std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - render_t0)
                                       .count()) +
                    " us");
    }
    rgba = base_rgba_;
    const auto to_px = [&](float wx, float wz, float& px, float& py) {
        float u = 0, v = 0;
        if (!MapPoint(m, wx, wz, u, v))
            return false;
        px = (u * b.width - crop_x0_) * scale + offx;
        py = (v * b.height - crop_y0_) * scale + offy;
        if (rev)
            px = w - px, py = h - py;
        return px >= 0 && py >= 0 && px < w && py < h;
    };
    {
        // Picture geometry for the sample thread (GPS view centre, people grouping).
        std::scoped_lock lock{map_mutex_};
        geo_.ok = true;
        geo_.image = m.image;
        geo_.tex_w = b.width, geo_.tex_h = b.height;
        geo_.crop_x0 = float(crop_x0_), geo_.crop_y0 = float(crop_y0_);
        geo_.scale = scale, geo_.offx = offx, geo_.offy = offy;
        geo_.reverse = rev;
    }
    for (const HubIcon& ic : m.icons) {
        // The selected person (People tab): a cyan ring under the marker (native CursorList teal).
        float px = 0, py = 0;
        if (!m.sel || ic.access != m.sel || !to_px(ic.x, ic.z, px, py))
            continue;
        for (int y = int(py - 44); y <= int(py + 44); ++y)
            for (int x = int(px - 44); x <= int(px + 44); ++x) {
                const float d = std::hypot(x + 0.5f - px, y + 0.5f - (py - 10));
                if (d > 30 && d < 38) {
                    const u8 c[4] = {0x96, 0xEB, 0xE6, 0xFF};
                    Blend(rgba, w, h, x, y, c, 1.f);
                } else if (d <= 30) {
                    const u8 c[4] = {0x96, 0xEB, 0xE6, 0xFF};
                    Blend(rgba, w, h, x, y, c, 0.28f);
                }
            }
    }
    const auto talk = Sprite(TalkBundle, "TalkIcon"); // 128x128: white/yellow top, cyan bottom
    for (const HubIcon& ic : m.icons) {
        if (!ic.active)
            continue;
        float px = 0, py = 0;
        if (!to_px(ic.x, ic.z, px, py))
            continue;
        const std::string icon = IconSprite(ic.icon);
        bool drew = false;
        if (!ic.dot.empty() && sprite_render_) {
            lp_unity::Image d;
            if (sprite_render_("dot/" + ic.dot, d.rgba, d.width, d.height) && d.width && d.height &&
                d.rgba.size() == std::size_t(d.width) * d.height * 4) {
                Stamp(rgba, w, h, d, 0, 0, int(d.width), int(d.height), px, py - 10, PeoplePx, 0, 1);
                drew = true;
            }
        }
        if (!icon.empty() && !drew)
            if (const auto spr = Sprite(MiniMapBundle, icon); spr && !spr->rgba.empty()) {
                // Native proportions: people / animals / pick-ups are small dots, facilities
                // larger tiles (hubmapicon.xml LargeScale 1.2 vs 2).
                const bool dot = ic.icon == "Unit" || ic.icon == "Mob1" || ic.icon == "Mob2" ||
                                 ic.icon == "AnimalPoint" || ic.icon == "ItemPoint";
                const float size = dot ? DotPx : FacilityPx;
                Stamp(rgba, w, h, *spr, 0, 0, int(spr->width), int(spr->height), px, py, size, 0, 1);
            }
        if (ic.talk && talk && talk->width >= 128) {
            const float a = (!ic.reliance && ic.talked == 1) ? 0.45f : 1.f;
            Stamp(rgba, w, h, *talk, 64, 0, 64, 64, px + 16, py - 26, TalkPx, 0, a);
        }
    }
    if (m.player) {
        float px = 0, py = 0;
        if (to_px(m.px, m.pz, px, py))
            if (const auto spr = Sprite(MiniMapBundle, "Lueur"); spr && !spr->rgba.empty())
                Stamp(rgba, w, h, *spr, 0, 0, int(spr->width), int(spr->height), px, py, PlayerPx, m.dir + (rev ? 180.f : 0.f), 1);
    }
    {
        std::scoped_lock lock{map_mutex_};
        rendered_.push_back(std::string{key});
        while (rendered_.size() > 8)
            rendered_.pop_front();
    }
    return true;
}
} // namespace fee_hub

namespace fee_hub {
std::string Hub::MascotShrine(const std::string& after, const std::string& name,
                              const std::string& fallback) {
    // MID_Hub_Mascot_After is "<name tag>'s Shrine" (en); the decoder drops the tag, leaving the
    // rest. Put the name where the tag was: at the front when the remainder starts with
    // punctuation/space ("'s Shrine"), else at the end ("Sanctuaire de ").
    if (name.empty() || after.empty() || fee_text::IsRawKey(after))
        return fallback;
    const unsigned char c0 = static_cast<unsigned char>(after.front());
    if (c0 == '\'' || c0 == ' ' || c0 >= 0x80)
        return name + after;
    if (after.back() == ' ')
        return after + name;
    return fallback;
}

std::string Hub::Localized(const std::string& key) const {
    // Never a raw key on screen: an undecoded key (table not loaded yet, a label missing) is "".
    if (key.empty() || !text_)
        return fee_text::IsRawKey(key) ? std::string{} : key;
    std::string v = text_(key);
    if (v.empty() || fee_text::IsRawKey(v))
        return fee_text::IsRawKey(key) ? std::string{} : key;
    for (char& c : v)
        if (c == '\n' || c == '\r')
            c = ' ';
    return v;
}

void Hub::ReadPlaces() {
    places_.clear();
    facilities_.clear();
    // Unlock gate: the main chapter the save is on (GameUserData.m_Chapter Cid "CID_M010");
    // a place/facility shows when its ConditionCID chapter is not later. Non-main chapter -> all.
    int current = 999;
    std::string mascot;
    if (const u64 gud = StaticPtr(TypeInfoGameUserData, 0))
        mascot = ManagedUtf8(Ptr(gud + GameUserDataMascotName).value_or(0), 32).value_or("");
    if (const u64 gud = StaticPtr(TypeInfoGameUserData, 0))
        if (const u64 chap = Ptr(gud + GameUserDataChapter).value_or(0)) {
            const std::string cid = ManagedString(Ptr(chap + ChapterCidField).value_or(0), 24).value_or("");
            if (cid.size() == 8 && cid.starts_with("CID_M"))
                current = std::atoi(cid.c_str() + 5);
        }
    const auto unlocked = [&](const std::string& cid) {
        if (cid.size() == 8 && cid.starts_with("CID_M"))
            return std::atoi(cid.c_str() + 5) <= current;
        return true;
    };
    const auto list_of = [&](u64 type_info, std::vector<u64>& rows) {
        const u64 list = StaticPtr(type_info, 0);
        const u64 items = list ? Ptr(list + pins::ListItems).value_or(0) : 0;
        const int n = list ? Get<s32>(list + pins::ListSize).value_or(0) : 0;
        for (int i = 0; items && i < n && i < 64; ++i)
            if (const u64 r = Ptr(items + pins::ArrayData + 8u * u64(i)).value_or(0))
                rows.push_back(r);
    };
    // AIDs are Japanese: compare the raw UTF-16 strings, not the ASCII-folded ones.
    const auto raw = [&](u64 str) {
        std::u16string out;
        const auto len = str ? Get<s32>(str + pins::StringLength) : std::nullopt;
        if (!len || *len <= 0 || *len > 64)
            return out;
        out.resize(std::size_t(*len));
        Read(str + pins::StringData, out.data(), out.size() * 2);
        while (!out.empty() && out.back() == u'\0')
            out.pop_back();
        return out;
    };
    std::vector<u64> areas, facilities;
    list_of(TypeInfoHubAreaData, areas);
    list_of(TypeInfoHubFacilityData, facilities);
    for (const u64 a : areas) {
        if (!unlocked(ManagedString(Ptr(a + AreaCondition).value_or(0), 24).value_or("")))
            continue;
        Place pl;
        pl.name = Localized(ManagedString(Ptr(a + AreaMid).value_or(0), 64).value_or(""));
        if (pl.name.empty())
            continue;
        const u64 arr = Ptr(a + AreaFacilities).value_or(0);
        const int n = arr ? int(Get<u32>(arr + pins::ArrayLength).value_or(0)) : 0;
        for (int k = 0; k < n && k < 8; ++k) {
            const std::u16string aid = raw(Ptr(arr + pins::ArrayData + 8u * u64(k)).value_or(0));
            for (const u64 f : facilities)
                if (raw(Ptr(f + FacilityAid).value_or(0)) == aid) {
                    if (unlocked(ManagedString(Ptr(f + FacilityCondition).value_or(0), 24).value_or(""))) {
                        auto icon = ManagedString(Ptr(f + FacilityIcon).value_or(0), 40);
                        if (icon && !icon->empty())
                            pl.icons.push_back(*icon);
                        Facility fa;
                        fa.aid = aid;
                        fa.icon = icon.value_or("");
                        const std::string mid = ManagedString(Ptr(f + FacilityMid).value_or(0), 64).value_or("");
                        fa.name = Localized(mid);
                        if (mid == "MID_Hub_Mascot" && !mascot.empty())
                            fa.name = MascotShrine(Localized("MID_Hub_Mascot_After"), mascot, fa.name);
                        fa.area = int(places_.size());
                        facilities_.push_back(std::move(fa));
                    }
                    break;
                }
        }
        places_.push_back(std::move(pl));
    }
}
} // namespace fee_hub
