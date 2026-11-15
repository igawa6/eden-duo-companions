// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mtas_reader.h"

#include "mtas_text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace mtas {

bool Reader::Read(u64 at, void* out, std::size_t n) const {
    return host_ && host_->read_memory && at >= 0x1000000 && at < (u64{1} << 48) &&
           host_->read_memory(host_->userdata, at, out, n) != EDEN_DSMOD_FALSE;
}

std::optional<s32> Reader::I32(u64 at) const {
    s32 v{};
    if (!Read(at, &v, 4))
        return std::nullopt;
    return v;
}

std::optional<u64> Reader::U64(u64 at) const {
    u64 v{};
    if (!Read(at, &v, 8))
        return std::nullopt;
    return v;
}

std::optional<float> Reader::F32(u64 at) const {
    float v{};
    if (!Read(at, &v, 4))
        return std::nullopt;
    return v;
}

bool Reader::Ok(u64 p) {
    return p >= 0x1000000 && p < (u64{1} << 48) && (p & 7) == 0;
}

bool Reader::OkUnaligned(u64 p) {
    return p >= 0x1000000 && p < (u64{1} << 48);
}

std::string Reader::CStr(u64 at, std::size_t max) const {
    char buf[128];
    if (!OkUnaligned(at) || max > sizeof buf)
        return {};
    for (std::size_t off = 0; off < max; off += 16) {
        if (!Read(at + off, buf + off, 16))
            return {};
        if (const void* z = std::memchr(buf + off, 0, off + 16 <= max ? 16 : max - off))
            return std::string(buf, static_cast<const char*>(z) - buf);
    }
    return {};
}

std::string Reader::ClassName(u64 klass) const {
    if (!Ok(klass))
        return {};
    const auto name_ptr = U64(klass + OffName);
    return name_ptr && OkUnaligned(*name_ptr) ? CStr(*name_ptr) : std::string{};
}

std::string Reader::DotNetString(u64 p, std::size_t max_chars) const {
    if (!Ok(p))
        return {};
    const auto n = I32(p + 0x10);
    if (!n || *n < 0 || static_cast<std::size_t>(*n) > max_chars)
        return {};
    if (*n == 0)
        return {};
    std::vector<char16_t> buf(static_cast<std::size_t>(*n));
    if (!Read(p + 0x14, buf.data(), buf.size() * 2))
        return {};
    return Utf16ToUtf8({buf.data(), buf.size()});
}

std::string Utf16ToUtf8(std::u16string_view buf) {
    std::string out;
    out.reserve(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i) {
        std::uint32_t cp = buf[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < buf.size() &&
            buf[i + 1] >= 0xDC00 && buf[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (buf[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else {
            const int bytes = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            out.push_back(static_cast<char>((bytes == 2 ? 0xC0 : bytes == 3 ? 0xE0 : 0xF0) |
                                           (cp >> (6 * (bytes - 1)))));
            for (int n = bytes - 2; n >= 0; --n)
                out.push_back(static_cast<char>(0x80 | ((cp >> (6 * n)) & 0x3F)));
        }
    }
    return out;
}

std::vector<u64> Reader::ListItems(u64 list_ptr, std::size_t limit) const {
    std::vector<u64> out;
    if (!Ok(list_ptr))
        return out;
    const auto items = U64(list_ptr + 0x10);
    const auto size = I32(list_ptr + 0x18);
    if (!items || !Ok(*items) || !size || *size < 0 || static_cast<std::size_t>(*size) > limit)
        return out;
    const auto cap = I32(*items + 0x18);
    if (!cap || *cap < *size || *cap > 65536)
        return out;
    std::vector<u64> raw(static_cast<std::size_t>(*size));
    if (!Read(*items + 0x20, raw.data(), raw.size() * 8))
        return out;
    return raw;
}

std::vector<std::pair<s32, s32>> Reader::ListIdCounts(u64 list_ptr, std::size_t limit) const {
    std::vector<std::pair<s32, s32>> out;
    if (!Ok(list_ptr))
        return out;
    const auto items = U64(list_ptr + 0x10);
    const auto size = I32(list_ptr + 0x18);
    if (!items || !Ok(*items) || !size || *size < 0 || static_cast<std::size_t>(*size) > limit)
        return out;
    const auto cap = I32(*items + 0x18);
    if (!cap || *cap < *size || *cap > 65536)
        return out;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(*size) * 8);
    if (!Read(*items + 0x20, raw.data(), raw.size()))
        return out;
    out.reserve(static_cast<std::size_t>(*size));
    for (std::size_t i = 0; i < static_cast<std::size_t>(*size); ++i) {
        s32 id{}, count{};
        std::memcpy(&id, raw.data() + i * 8, 4);
        std::memcpy(&count, raw.data() + i * 8 + 4, 4);
        out.emplace_back(id, count);
    }
    return out;
}

std::vector<s32> Reader::ListInt32s(u64 list_ptr, std::size_t limit) const {
    std::vector<s32> out;
    if (!Ok(list_ptr))
        return out;
    const auto items = U64(list_ptr + 0x10);
    const auto size = I32(list_ptr + 0x18);
    if (!items || !Ok(*items) || !size || *size < 0 || static_cast<std::size_t>(*size) > limit)
        return out;
    const auto cap = I32(*items + 0x18);
    if (!cap || *cap < *size || *cap > 65536)
        return out;
    std::vector<s32> raw(static_cast<std::size_t>(*size));
    if (!Read(*items + 0x20, raw.data(), raw.size() * 4))
        return out;
    return raw;
}

std::optional<std::vector<s32>> Reader::ListInt32sStrict(u64 list_ptr, std::size_t limit) const {
    if (!Ok(list_ptr))
        return std::nullopt;
    const auto items = U64(list_ptr + 0x10);
    const auto size = I32(list_ptr + 0x18);
    if (!items || !Ok(*items) || !size || *size < 0 || static_cast<std::size_t>(*size) > limit)
        return std::nullopt;
    const auto cap = I32(*items + 0x18);
    if (!cap || *cap < *size || *cap > 65536)
        return std::nullopt;
    std::vector<s32> out(static_cast<std::size_t>(*size));
    if (!out.empty() && !Read(*items + 0x20, out.data(), out.size() * 4))
        return std::nullopt;
    return out;
}

std::vector<std::pair<s32, u64>> Reader::DictEntries(u64 dict_ptr, std::size_t limit) const {
    std::vector<std::pair<s32, u64>> out;
    if (!Ok(dict_ptr))
        return out;
    const auto entries = U64(dict_ptr + 0x18);
    const auto count = I32(dict_ptr + 0x20);
    if (!entries || !Ok(*entries) || !count || *count < 0 ||
        static_cast<std::size_t>(*count) > limit)
        return out;
    const auto cap = I32(*entries + 0x18);
    if (!cap || *cap < *count || *cap > 65536)
        return out;
    constexpr std::size_t Stride = 0x18;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(*count) * Stride);
    if (!Read(*entries + 0x20, raw.data(), raw.size()))
        return out;
    out.reserve(static_cast<std::size_t>(*count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(*count); ++i) {
        s32 hash_code{};
        std::memcpy(&hash_code, raw.data() + i * Stride, 4);
        if (hash_code < 0)
            continue; // free-list slot (removed/never-used), matches upstream BCL semantics
        s32 key{};
        u64 value{};
        std::memcpy(&key, raw.data() + i * Stride + 0x08, 4);
        std::memcpy(&value, raw.data() + i * Stride + 0x10, 8);
        out.emplace_back(key, value);
    }
    return out;
}

std::string Reader::ItemName(s32 item_id) {
    return GameText().ItemName(item_id); // "" if the table hasn't loaded or the id is unknown
}

std::optional<u64> Reader::ResolveRoot(u64 main_base, u64 slot, const char* expect_name,
                                       int hops) const {
    const auto klass = U64(main_base + slot);
    if (!klass || !Ok(*klass))
        return std::nullopt;
    // PERF cache hit: the klass pointer at this TypeInfo slot and its own name pointer are part
    // of the type system (not per-save instance data), so re-matching both against the cached
    // values is a cheap, sufficient proof that the rest of the cached chain (hops walk +
    // static_fields) is still correct, without re-deriving any of it.
    if (const auto it = root_cache_.find(slot); it != root_cache_.end()) {
        const auto name_ptr = U64(*klass + OffName);
        if (*klass == it->second.klass && name_ptr && *name_ptr == it->second.name_ptr)
            return it->second.statics;
        root_cache_.erase(it); // stale -- fall through and re-resolve fully
    }
    const auto name_ptr = U64(*klass + OffName);
    if (ClassName(*klass) != expect_name)
        return std::nullopt;
    u64 cur = *klass;
    for (int i = 0; i < hops; ++i) {
        const auto parent = U64(cur + OffParent);
        if (!parent || !Ok(*parent))
            return std::nullopt;
        cur = *parent;
    }
    const auto statics = U64(cur + OffStaticFields);
    if (!statics || !Ok(*statics))
        return std::nullopt;
    if (name_ptr)
        root_cache_[slot] = {*klass, *name_ptr, *statics};
    return *statics;
}

Calendar CalendarFromDayOfWorld(u64 day_of_world) {
    static constexpr const char* kSeasons[4] = {"Spring", "Summer", "Autumn", "Winter"};
    static constexpr const char* kWeekdays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                 "Thursday", "Friday", "Saturday"};
    const u64 day_of_year = day_of_world % 112;
    Calendar c;
    c.day_of_world = day_of_world;
    c.year = static_cast<s32>(day_of_world / 112 + 1);
    c.month = static_cast<s32>(day_of_year / 28 + 1);
    c.day_in_season = static_cast<s32>(day_of_year % 28 + 1);
    c.season = kSeasons[day_of_year / 28];
    c.weekday = kWeekdays[(day_of_world + 1) % 7];
    return c;
}

Calendar CalendarFromDateData(u64 date_data) {
    constexpr u64 TicksMask = 0x3FFFFFFFFFFFFFFFULL;
    constexpr u64 TicksPerDay = 24ULL * 60 * 60 * 1000 * 10000;
    const u64 ticks = date_data & TicksMask;
    return CalendarFromDayOfWorld(ticks / TicksPerDay);
}

TimeFields Reader::ReadTime(u64 main_base) const {
    TimeFields out;
    const auto statics = ResolveRoot(main_base, SlotTimeModule, "TimeModule", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto day = I32(*self + 0x58);
    const auto hour = I32(*self + 0x5C);
    const auto minute = I32(*self + 0x60);
    if (!day || !hour || !minute)
        return out;
    out.ok = true;
    out.day = *day;
    out.hour = *hour;
    out.minute = *minute;

    // CurrentTime.dateData (GameDateTime, a plain ulong backing field, self+0x50). Top 2 bits are
    // Kind (System.DateTime scheme); low 62 bits are ticks, 10,000,000/sec. This build's calendar
    // is 4 "months" (seasons) of 28 days/year, verified live against the native HUD/Calendar
    // across two timestamps a day apart.
    const auto date_data = U64(*self + 0x50);
    if (!date_data)
        return out;
    const Calendar c = CalendarFromDateData(*date_data);
    out.calendar_ok = true;
    out.day_of_world = c.day_of_world;
    out.year = c.year;
    out.month = c.month;
    out.season = c.season;
    out.weekday = c.weekday;
    // WeatherModule.CurWeather, same supported build and validated generic root as TimeModule.
    if (const auto fields = ResolveRoot(main_base, 0x80B9228, "WeatherModule", 2)) {
        const auto weather_self = U64(*fields);
        const auto weather = weather_self && Ok(*weather_self) ? I32(*weather_self + 0x34) : std::nullopt;
        if (weather && *weather >= 0 && *weather < 6) {
            out.weather_ok = true;
            out.weather = *weather;
        }
    }
    return out;
}

TimeLeft MissionTimeLeft(u64 current_date_data, std::int64_t submit_over_time) {
    constexpr u64 TicksMask = 0x3FFFFFFFFFFFFFFFULL;
    constexpr u64 TicksPerDay = 24ULL * 60 * 60 * 1000 * 10000;
    constexpr u64 TicksPerHour = TicksPerDay / 24;
    TimeLeft out;
    if (submit_over_time <= 0)
        return out; // sentinel/absent: this mission has no deadline (e.g. the groupType==-1
                    // story mission observed live)
    const u64 deadline_ticks = static_cast<u64>(submit_over_time) & TicksMask;
    const u64 now_ticks = current_date_data & TicksMask;
    if (deadline_ticks <= now_ticks) {
        out.has_deadline = true;
        out.overdue = true;
        return out;
    }
    const u64 left = deadline_ticks - now_ticks;
    out.has_deadline = true;
    out.days = static_cast<s32>(left / TicksPerDay);
    out.hours = static_cast<s32>((left % TicksPerDay) / TicksPerHour);
    return out;
}

BagFields Reader::ReadBag(u64 main_base) const {
    BagFields out;
    const auto statics = ResolveRoot(main_base, SlotPlayer, "Player", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto bag = U64(*self + 0x1B0); // <bag>k__BackingField
    if (!bag || !Ok(*bag))
        return out;
    const auto gold = I32(*bag + 0xA8);
    if (!gold)
        return out;
    out.ok = true;
    out.gold = *gold;
    return out;
}

UiFields Reader::ReadUi(u64 main_base) const {
    UiFields out;
    const auto klass = U64(main_base + SlotUiSystemMgr);
    if (!klass || !Ok(*klass))
        return out;
    if (ClassName(*klass) != "UISystemMgr")
        return out;
    const auto statics = U64(*klass + OffStaticFields);
    if (!statics || !Ok(*statics))
        return out;
    const auto instance = U64(*statics + 0x28); // <Instance>k__BackingField
    if (!instance || !Ok(*instance))
        return out;
    const auto layer = U64(*instance + 0xB0); // curDisplayLayer
    if (!layer || !Ok(*layer))
        return out;
    const auto peek = U64(*layer + 0x70); // PeekState
    if (!peek || !Ok(*peek))
        return out;
    const auto state_type = I32(*peek + 0x84);
    if (!state_type)
        return out;
    out.ok = true;
    out.state_type = *state_type;
    return out;
}

DialogueFields Reader::ReadDialogueSpeaker(u64 main_base) const {
    DialogueFields out;
    const auto klass = U64(main_base + SlotUiSystemMgr);
    if (!klass || !Ok(*klass))
        return out;
    if (ClassName(*klass) != "UISystemMgr")
        return out;
    const auto statics = U64(*klass + OffStaticFields);
    if (!statics || !Ok(*statics))
        return out;
    const auto instance = U64(*statics + 0x28); // <Instance>k__BackingField
    if (!instance || !Ok(*instance))
        return out;
    const auto layer = U64(*instance + 0xB0); // curDisplayLayer
    if (!layer || !Ok(*layer))
        return out;
    const auto peek = U64(*layer + 0x70); // PeekState (UIAssetBase)
    if (!peek || !Ok(*peek))
        return out;
    const auto ui_ref = U64(*peek + 0x50); // UIAssetBase.<uiRef>k__BackingField (IUIChange)
    if (!ui_ref || !Ok(*ui_ref))
        return out; // no UI instantiated for the current state push yet (e.g. mid-transition)
    const auto ui_ref_klass = U64(*ui_ref);
    if (!ui_ref_klass || !Ok(*ui_ref_klass))
        return out;
    if (ClassName(*ui_ref_klass) != "DialogUI")
        return out; // whatever's on screen isn't a dialogue box (e.g. GamingUI in free roam)
    const auto dialog_name_ptr = U64(*ui_ref + 0x78); // DialogUI.dialogName (TextMeshProUGUI)
    if (!dialog_name_ptr || !Ok(*dialog_name_ptr))
        return out;
    const auto text_ptr = U64(*dialog_name_ptr + 0xD0); // TMP_Text.m_text
    if (!text_ptr || !Ok(*text_ptr))
        return out;
    const std::string speaker = DotNetString(*text_ptr, 64);
    if (speaker.empty())
        return out;
    out.ok = true;
    out.speaker_name = speaker;
    out.npc_id = GameText().NpcIdByName(speaker); // -1 if no met/unmet NPC has this exact name
                                                  // (the player's own self-talk, "Max")
    return out;
}

/// Round-2: see mtas_reader.h's own comment on SocialHearts/ComputeSocialHearts for the native
/// pip-strip evidence this table is built from.
SocialHearts ComputeSocialHearts(s32 social_type, s32 level) {
    SocialHearts out;
    if (social_type < 0 || level < 0) { // SocialType.None(-1) / SocialLevel.None(-1)
        out.stranger = true;
        return out;
    }
    struct Range {
        s32 type, lo, hi;
    };
    static constexpr Range kRanges[] = {
        {0, 2, 8},   // Normal: NormalStranger..FriendBest (0/1 Nemesis/Adversary excluded, see header)
        {1, 9, 12},  // Lover: Lover..LoverDeepest
        {2, 13, 16}, // Mate: MateUnhappy..MateHappiest
        {3, 17, 20}, // Child: ChildStranger..ChildHappy
        {4, 21, 24}, // Pet: PetStranger..PetLoyal
    };
    for (const auto& r : kRanges) {
        if (r.type != social_type)
            continue;
        out.hearts_max = r.hi - r.lo + 1;
        // +1: the family's own floor level (e.g. NormalStranger=2) already shows one filled pip
        // (an established, if minimal, track position), and the top level (FriendBest=8) shows
        // every pip filled. Live-verified for the Normal family this round, 11 NPCs, one boot --
        // DATA-CONTRACT.md "Social hearts" has the full table; this header's own comment above has
        // the summary.
        out.hearts = std::clamp(level - r.lo + 1, 0, out.hearts_max);
        return out;
    }
    return out; // unrecognised social_type: no pip track to show, not a guess
}

s32 DaysUntilBirthday(u64 today, s32 birthday_month, s32 birthday_day) {
    if (birthday_month < 1 || birthday_month > 4 || birthday_day < 1 || birthday_day > 28)
        return -1;
    constexpr u64 kYearDays = 112; // 4 seasons * 28 days, same constant CalendarFromDayOfWorld uses
    const u64 day_in_year =
        static_cast<u64>(birthday_month - 1) * 28 + static_cast<u64>(birthday_day - 1);
    const u64 this_year_base = (today / kYearDays) * kYearDays;
    u64 candidate = this_year_base + day_in_year;
    if (candidate < today)
        candidate += kYearDays; // already passed this year: next occurrence is next year
    return static_cast<s32>(candidate - today);
}

/// Round-4 follow-up: see mtas_reader.h's own comment on ComputeMissionClass for the full live
/// evidence (3 real examples against the native coloured-dot badges, one boot).
MissionClass ComputeMissionClass(std::optional<bool> base_info_is_main, s32 group_type) {
    if (base_info_is_main)
        return *base_info_is_main ? MissionClass::Main : MissionClass::Side;
    // No MissionBaseInfo entry at all: an Order/Repair/Sand/Monster mission, not a "Normal"
    // template. CommercialOrder=0, TownOrder=1: the two order-board group types observed live
    // ("The Request Board", "Sandrock Commission", both groupType==0) -- the Commission/
    // Request-Board system (ASSET-PATHS.md §4).
    if (group_type == 0 || group_type == 1)
        return MissionClass::Commission;
    return MissionClass::Other;
}

std::optional<s32> Reader::ReadCurrentMissionTargetTitleId(u64 mission_ptr) const {
    const auto list_ptr = U64(mission_ptr + 0x78); // m_missionTargetList
    if (!list_ptr || !Ok(*list_ptr))
        return std::nullopt;
    // MissionPartControl.GetTargetName (0x22C2F90) calls target.GetTitle(). The base
    // implementation (0x38AA470) resolves title at +0x1C, not the mission synopsis at +0xF0.
    // Native IsTargetComplete (0x22C32C0) checks state +0x34 == 1 (Accomplished).
    // Running == 0: skip completed/failed steps instead of showing a checked-off objective.
    for (const u64 target : ListItems(*list_ptr, 64)) {
        if (!Ok(target))
            continue;
        const auto state = I32(target + 0x34);
        if (!state || *state != 0)
            continue;
        const auto title_id = I32(target + 0x1C);
        return title_id && *title_id > 0 ? title_id : std::nullopt;
    }
    return std::nullopt;
}

ItemProgress Reader::ReadMissionItemProgress(u64 mission_ptr) const {
    ItemProgress out;
    const auto item_id = I32(mission_ptr + 0x118);   // itemOrMonsterId
    const auto item_need = I32(mission_ptr + 0x11C);  // itemOrMonsterCount
    if (!item_id || *item_id <= 0 || !item_need)
        return out;
    out.ok = true;
    out.item_id = *item_id;
    out.need = *item_need;
    if (const auto grade = I32(mission_ptr + 0x120); grade && *grade >= -1 && *grade <= 3)
        out.grade = *grade;
    out.item_name = ItemName(*item_id);
    // Current progress: Mission.m_missionTargetList (self+0x78) -> first MissionTargetBase ->
    // its own `progress` field (target+0x40, List<IdCount>) -> first entry's `count`. Only
    // resolves once the mission has been traced (StartTrace()/InitProgress() populates this) --
    // empty for every other running-list mission (DATA-CONTRACT.md "Mission item lines").
    if (const auto target_list = U64(mission_ptr + 0x78); target_list && Ok(*target_list)) {
        const auto targets = ListItems(*target_list, 8);
        if (!targets.empty() && Ok(targets.front())) {
            if (const auto progress_list = U64(targets.front() + 0x40);
                progress_list && Ok(*progress_list)) {
                const auto progress = ListIdCounts(*progress_list, 8);
                if (!progress.empty()) {
                    out.have_ok = true;
                    out.have = progress.front().second;
                }
            }
        }
    }
    return out;
}

/// Round-4 follow-up: MissionManager.allMissionBaseInfoById (self+0x20, Dictionary<int,
/// MissionBaseInfo>) -- live-confirmed 1,069 entries this build, EVERY one reading
/// MissionBaseInfo.GroupType==Normal(-1) (this dictionary is the "Normal" mission-template
/// registry specifically; Order/Repair/Sand/Monster missions live in their own type-specific
/// lists, e.g. `recentDeliverOrder`/`activeOrder`, not here). DictEntries' own value is the
/// MissionBaseInfo pointer, left undereferenced so ComputeMissionClass's callers only pay for an
/// IsMain read on the handful of ids they actually have. PERF: cached behind a cheap fingerprint
/// (the dictionary's own pointer + entry count) -- this function has two callers (ReadMissions,
/// ReadTracked), both run every Refresh(), which used to rebuild this whole ~1,069-entry map from
/// scratch twice per tick; a static per-template registry like this one never actually changes
/// mid-boot, so in practice the full rebuild now happens once, the very first time either caller
/// runs, not every call.
std::unordered_map<s32, u64> Reader::ReadMissionBaseInfoIndex(u64 main_base) const {
    const auto statics = ResolveRoot(main_base, SlotMissionManager, "MissionManager", 3);
    if (!statics)
        return {}; // chain unresolved this sample -- never serve a stale cache as if it resolved
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return {};
    const auto dict_ptr = U64(*self + 0x20); // allMissionBaseInfoById
    if (!dict_ptr || !Ok(*dict_ptr))
        return {};
    const auto count = I32(*dict_ptr + 0x20);
    if (!count || *count < 0)
        return {};
    if (*dict_ptr == mission_base_info_dict_ && *count == mission_base_info_count_)
        return mission_base_info_cache_;
    std::unordered_map<s32, u64> out;
    for (const auto& [key, vptr] : DictEntries(*dict_ptr, 2048))
        out.emplace(key, vptr);
    mission_base_info_dict_ = *dict_ptr;
    mission_base_info_count_ = *count;
    mission_base_info_cache_ = out;
    return out;
}

std::optional<bool> Reader::MissionBaseInfoIsMain(const std::unordered_map<s32, u64>& index,
                                                  s32 mission_id) const {
    const auto it = index.find(mission_id);
    if (it == index.end() || !Ok(it->second))
        return std::nullopt;
    std::uint8_t b{};
    if (!Read(it->second + 0x18, &b, 1)) // MissionBaseInfo.IsMain
        return std::nullopt;
    return b != 0;
}

MissionsFields Reader::ReadMissions(u64 main_base, u64 current_date_data, bool have_clock) const {
    MissionsFields out;
    const auto statics = ResolveRoot(main_base, SlotMissionManager, "MissionManager", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    // MissionPartControl.InitMissionList (0x22C00E0) reads this same running list.
    // +0xC0 is m_missions_Actived: offers that have not been accepted.
    const auto list_ptr = U64(*self + 0xC8); // m_missions_Running
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    const auto items = ListItems(*list_ptr, 64);
    const auto base_info_index = ReadMissionBaseInfoIndex(main_base);
    out.ok = true; // the chain resolved even if 0 missions are running right now
    for (const u64 mission_ptr : items) {
        if (!Ok(mission_ptr))
            continue;
        MissionFields mf;
        if (const auto id = I32(mission_ptr + 0x10))
            mf.mission_id = *id;
        if (const auto name_ptr = U64(mission_ptr + 0xE8)) // <MissionNameStr>k__BackingField
            mf.title = DotNetString(*name_ptr);
        s32 group_type = 0;
        if (const auto gt = I32(mission_ptr + 0x128)) {
            group_type = *gt;
            mf.group_type = *gt;
        }
        const auto base_info_is_main = MissionBaseInfoIsMain(base_info_index, mf.mission_id);
        mf.mission_class = static_cast<std::uint8_t>(ComputeMissionClass(base_info_is_main, group_type));
        if (const auto title_id = ReadCurrentMissionTargetTitleId(mission_ptr))
            mf.objective = GameText().Get(*title_id);
        const ItemProgress item = ReadMissionItemProgress(mission_ptr);
        mf.item_ok = item.ok;
        mf.item_name = item.item_name;
        mf.item_need = item.need;
        mf.item_grade = item.grade;
        mf.item_have_ok = item.have_ok;
        mf.item_have = item.have;
        if (item.ok)
            mf.item_icon = GameText().ItemIcon(item.item_id);
        if (have_clock) {
            // Native GetCompletionTime (0x22C47EC..0x22C4810) uses submitOverTime.
            if (const auto submit_over = U64(mission_ptr + 0x48))
                mf.left = MissionTimeLeft(current_date_data, static_cast<std::int64_t>(*submit_over));
        }
        out.running.push_back(std::move(mf));
    }
    return out;
}

/// MissionManager.traceList (self+0xE0, `private List<Mission>`) -- the list the native HUD
/// mission-tracker banner (MissionTraceUI/MissionTraceControl) actually displays, found live this
/// pass by reading every MissionManager field dump.cs declares and cross-checking against the HUD
/// text: `mainMission` (self+0x128, a single `Mission` field) read null every boot; `traceList`
/// held exactly one entry whose title ("Sandrock Commission"), item id/need (itemOrMonsterId=
/// 19800019 "Bronze Stick", itemOrMonsterCount=6) and current progress (target.progress[0].count
/// =5) matched the HUD's "Sandrock Commission / Complete the order / Bronze Stick (5/6)" exactly
/// -- see DATA-CONTRACT.md. Replaces the old groupType==-1 heuristic entirely (that heuristic
/// only ever matched "Gone with the Wind", which the HUD does NOT show as tracked).
TrackedMission Reader::ReadTracked(u64 main_base, u64 current_date_data, bool have_clock) const {
    TrackedMission out;
    const auto statics = ResolveRoot(main_base, SlotMissionManager, "MissionManager", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto list_ptr = U64(*self + 0xE0); // traceList
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    const auto items = ListItems(*list_ptr, 8);
    if (items.empty() || !Ok(items.front()))
        return out;
    const u64 mp = items.front(); // the HUD shows one tracked mission at a time on this build
    out.ok = true;
    if (const auto name_ptr = U64(mp + 0xE8))
        out.title = DotNetString(*name_ptr);
    s32 group_type = 0;
    if (const auto gt = I32(mp + 0x128)) {
        group_type = *gt;
        out.group_type = *gt;
    }
    s32 mission_id = 0;
    if (const auto id = I32(mp + 0x10))
        mission_id = *id;
    const auto base_info_index = ReadMissionBaseInfoIndex(main_base);
    const auto base_info_is_main = MissionBaseInfoIsMain(base_info_index, mission_id);
    out.mission_class = static_cast<std::uint8_t>(ComputeMissionClass(base_info_is_main, group_type));
    if (have_clock) {
        if (const auto submit_over = U64(mp + 0x48)) // submitOverTime, as on the Mission page
            out.left = MissionTimeLeft(current_date_data, static_cast<std::int64_t>(*submit_over));
    }
    out.item = ReadMissionItemProgress(mp);
    return out;
}

/// SocialModule.mFavorDatas (self+0xA0, Dictionary<int,FavorData>) filtered to mKnownList
/// (self+0xC0, List<int> -- SocialModule.IsKnownNpc's own backing list, the native Social tab's
/// "met" gate: re/il2cpp-dump/dump.cs declares both on SocialModule, TypeDefIndex 12844).
/// FavorData (dump.cs TypeDefIndex 12763): mId@0x10, mFavor@0x14, mType@0x24 (SocialType),
/// mLevel@0x28 (SocialLevel), mLevelMax@0x2C. `next_level_points`/`level_max` come from
/// GameText().NextLevelInfo (the real SocialLevelConfig favor-range table), not from mLevelMax
/// (which is a high-water mark, not a hard cap -- see DATA-CONTRACT.md "Social").
SocialFields Reader::ReadSocial(u64 main_base) const {
    SocialFields out;
    const auto statics = ResolveRoot(main_base, SlotSocialModule, "SocialModule", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto known_list = U64(*self + 0xC0); // mKnownList
    if (!known_list || !Ok(*known_list))
        return out;
    const auto known_ids = ListInt32s(*known_list, 256);
    const auto dict_ptr = U64(*self + 0xA0); // mFavorDatas
    std::vector<std::pair<s32, u64>> entries;
    if (dict_ptr && Ok(*dict_ptr))
        entries = DictEntries(*dict_ptr, 256);
    std::unordered_map<s32, u64> by_id;
    by_id.reserve(entries.size());
    for (const auto& [id, ptr] : entries)
        by_id.emplace(id, ptr);

    out.ok = true;
    out.met_count = static_cast<s32>(known_ids.size());
    for (const s32 npc_id : known_ids) {
        const auto it = by_id.find(npc_id);
        if (it == by_id.end() || !Ok(it->second))
            continue;
        const u64 fav_ptr = it->second;
        NpcFields nf;
        nf.npc_id = npc_id;
        nf.name = GameText().NpcName(npc_id);
        // Assets keep their base names when display names are translated.
        nf.portrait = GameText().NpcPortrait(npc_id);
        const auto favor = I32(fav_ptr + 0x14);
        const auto type = I32(fav_ptr + 0x24);  // mType
        const auto level = I32(fav_ptr + 0x28); // mLevel
        if (favor)
            nf.favor = *favor;
        if (level)
            nf.level = *level;
        if (type && level) {
            nf.social_type = *type;
            nf.level_title = GameText().SocialLevelTitle(*type, *level);
            const SocialHearts hearts = ComputeSocialHearts(*type, *level);
            nf.hearts = hearts.hearts;
            nf.hearts_max = hearts.hearts_max;
            nf.stranger = hearts.stranger;
        }
        if (favor && type && level) {
            const auto info = GameText().NextLevelInfo(*type, *level, *favor);
            if (info.ok) {
                nf.level_max = info.maxed;
                nf.next_level_points = info.points_needed;
            }
        }
        const auto bday = GameText().NpcBirthdayOf(npc_id);
        if (bday.ok) {
            nf.birthday_ok = true;
            nf.birthday_month = bday.month;
            nf.birthday_day = bday.day;
        }
        // chatted/gifted today: no such field exists on FavorData (dump.cs's own field list,
        // confirmed, matches RE-REPORT.md's prior finding) -- left ok=false, not guessed.
        out.npcs.push_back(std::move(nf));
    }
    return out;
}

bool MatchesInventoryCategory(const InvItem& item, s32 filter) {
    if (filter == 0)
        return true;
    if (!item.category_ok)
        return false;
    if (filter == 1 || filter == 9)
        return item.collection == (filter == 1 ? 1 : 2);
    // CheckItem 0x1C68420 -> HasTag 0x2725C40 tests both live masks.
    const s32 tag = filter >= 2 && filter <= 4 ? filter :
                    filter == 5 ? 1 : filter == 6 ? 0 : -1;
    return tag >= 0 && ((item.prototype_tags | item.instance_tags) & (std::uint32_t{1} << tag)) != 0;
}

/// Player.bag (self+0x1B0) -> storage (PlayerBagItemContainer, +0xC8) -> slots
/// (List<ReadOnlyItemInstanceInBag>, +0x78): exact 190/192 cross-check, LIVE-READS.md §4.
/// Separate player-placed storage furniture (StorageUnit via Unit<StorageUnit>, dump.cs) is a
/// scene-object system, not a flat module field; its own live list root is resolved separately by
/// ReadStorage into StorageFields, then merged back into InventoryFields by MergeStorageItems
/// (see that function's own comment) -- this function itself only ever walks the one container
/// it's handed (the bag here, one storage box per call from ReadStorage).
std::vector<InvItem> Reader::ReadItemContainerSlots(u64 container_ptr, bool in_storage) const {
    std::vector<InvItem> out;
    if (!Ok(container_ptr))
        return out;
    const auto slots_list = U64(container_ptr + 0x78); // ItemContainer.slots
    if (!slots_list || !Ok(*slots_list))
        return out;
    for (const u64 wrapper_ptr : ListItems(*slots_list, 256)) {
        if (!Ok(wrapper_ptr))
            continue;
        const auto item_ptr = U64(wrapper_ptr + 0x18); // ReadOnlyItemInstance.item
        if (!item_ptr || !Ok(*item_ptr))
            continue; // empty slot
        InvItem it;
        it.in_storage = in_storage;
        const auto proto_ptr = U64(*item_ptr + 0x18); // ItemInstance.prototype
        if (proto_ptr && Ok(*proto_ptr)) {
            const auto prototype_tags = I32(*proto_ptr + 0x10);
            const auto instance_tags = I32(*item_ptr + 0x10);
            const auto collection = I32(*item_ptr + 0x34);
            if (prototype_tags && instance_tags && collection) {
                it.prototype_tags = static_cast<std::uint32_t>(*prototype_tags);
                it.instance_tags = static_cast<std::uint32_t>(*instance_tags);
                it.collection = *collection;
                it.category_ok = true;
            }
            if (const auto id = I32(*proto_ptr + 0x14)) {
                it.item_id = *id;
                it.name = GameText().ItemName(*id);
                it.icon = GameText().ItemIcon(*id);
            }
        }
        if (const auto count = I32(*item_ptr + 0x28))
            it.count = *count;
        if (const auto grade = I32(*item_ptr + 0x30))
            it.grade = *grade;
        out.push_back(std::move(it));
    }
    return out;
}

InventoryFields Reader::ReadInventory(u64 main_base) const {
    InventoryFields out;
    const auto statics = ResolveRoot(main_base, SlotPlayer, "Player", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto bag = U64(*self + 0x1B0);
    if (!bag || !Ok(*bag))
        return out;
    const auto storage = U64(*bag + 0xC8); // <storage>k__BackingField
    if (!storage || !Ok(*storage))
        return out;
    const auto slots_list = U64(*storage + 0x78); // ItemContainer.slots
    if (!slots_list || !Ok(*slots_list))
        return out;
    out.ok = true;
    out.bag_slots = static_cast<s32>(ListItems(*slots_list, 256).size());
    out.items = ReadItemContainerSlots(*storage, /*in_storage=*/false);
    out.bag_used = static_cast<s32>(out.items.size());
    return out;
}

/// GuildRankingManager.sortedWorkshopList (self+0x20, List<WorkshopReputation>): exact
/// cross-check, LIVE-READS.md §5. WorkshopReputation (dump.cs, abstract base): mType@0x10,
/// id@0x14, level@0x18, curValue@0x1C, lastUpgradeValue@0x20, curUpgradeValue@0x24. Competitor
/// names: CompetitorWorkshopRep.refData (self+0x38, WorkshopCompetitorData*) ->
/// WorkshopCompetitorData.workshopName (a text id, +0x14) -> GameText().Get(). `rank_display` =
/// level+1, reasoned from the task's own example (level=2 live, native shows "Rank 3") --
/// treated as live-verifiable, not yet independently re-confirmed this session beyond that one
/// documented example; see DATA-CONTRACT.md.
WorkshopFields Reader::ReadWorkshop(u64 main_base) const {
    WorkshopFields out;
    const auto statics = ResolveRoot(main_base, SlotGuildRankingManager, "GuildRankingManager", 2);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto list_ptr = U64(*self + 0x20); // sortedWorkshopList
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    const auto items = ListItems(*list_ptr, 32);
    out.ok = true;
    for (const u64 ptr : items) {
        if (!Ok(ptr))
            continue;
        std::string cname;
        if (const auto klass = U64(ptr); klass && Ok(*klass))
            cname = ClassName(*klass);
        CompetitorRep rep;
        rep.is_player = (cname == "PlayerWorkshopRep");
        if (const auto level = I32(ptr + 0x18))
            rep.level = *level;
        if (const auto cur = I32(ptr + 0x1C))
            rep.value = *cur;
        if (rep.is_player) {
            out.rank_display = rep.level + 1;
            out.rep_value = rep.value;
            if (const auto cur_upgrade = I32(ptr + 0x24))
                out.rep_next = *cur_upgrade;
        } else if (const auto ref_data = U64(ptr + 0x38); ref_data && Ok(*ref_data)) {
            if (const auto name_id = I32(*ref_data + 0x14))
                rep.name = GameText().Get(*name_id);
        }
        out.ranking.push_back(std::move(rep));
    }
    std::sort(out.ranking.begin(), out.ranking.end(),
             [](const CompetitorRep& a, const CompetitorRep& b) { return a.value > b.value; });
    return out;
}

/// DATA LANE round 3: resolves the RE-REPORT.md/LIVE-READS.md §5 "BLOCKED" gap -- the earlier
/// guess (ActorAttr.modifiableAttrDatas, indexed by ActorAttrType) was the wrong lead; the real
/// backing store is Actor.attrData (AttrData.exp/level, both plain public int fields, dump.cs
/// Pathea.ActorNs.AttrData TypeDefIndex 12478), reached via Player.actor (self+0x38), not through
/// ActorAttr/EnumModifiableAttr at all. See mtas_reader.h's own comment for every offset.
PlayerFields Reader::ReadPlayer(u64 main_base) const {
    PlayerFields out;
    const auto statics = ResolveRoot(main_base, SlotPlayer, "Player", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto actor = U64(*self + 0x38); // <actor>k__BackingField
    if (!actor || !Ok(*actor))
        return out;
    const auto attr_data = U64(*actor + 0x40); // <attrData>k__BackingField
    if (!attr_data || !Ok(*attr_data))
        return out;
    const auto exp = I32(*attr_data + 0x10);
    const auto level = I32(*attr_data + 0x14);
    if (!exp || !level)
        return out;
    out.ok = true;
    out.exp = *exp;
    out.level = *level;
    if (const auto attr_cmpt = U64(*actor + 0x78); attr_cmpt && Ok(*attr_cmpt)) {
        std::uint32_t bits{};
        if (Read(*attr_cmpt + 0x28, &bits, 4)) {
            if (bits == 0x7F800000u) {
                // AttrCmpt.get_NextLevelExp()'s own +Infinity sentinel: max level, no next --
                // reproduced from the disassembled getter (DATA-CONTRACT.md), not guessed.
                out.exp_ok = true;
                out.exp_next = out.exp; // "no further EXP needed": next == current, never invented
            } else {
                float f{};
                std::memcpy(&f, &bits, 4);
                if (f >= 0.0f && f < 1.0e9f) { // sane positive range, not a garbage float
                    out.exp_ok = true;
                    out.exp_next = static_cast<s32>(f);
                }
            }
        }
    }
    return out;
}

/// MachineModule.playerMachines (self+0x38, List<MachineBase>, 4-hop walk via
/// ItemSystemExtendArchivableUpdatableModule<T,Prototype>): STATUS.md §13 Fix 5,
/// tools/probe_machines.py. MachineBase (dump.cs): Status@0x28, Output@0x30, supports (raw
/// MachineSupport[] array, NOT a List<T>) @0x48. MachineStatus: IsProducting (bool) @0x11.
/// MachineOutput: items (List<ItemInstance>) @0x18 -- non-empty means a finished product is
/// waiting to be collected (the native machine-alert icon). MachineSupport (dump.cs abstract
/// base): CurrentValue @0x20, maximumValueFinal @0x1C -- Fix 5's probe read the unmodified base
/// `maximumValue` @0x18 instead (always 0: it is only ever set by the ctor's default and
/// overwritten at runtime via ModifyMaximumValue()/UpdateMaximumValue() into the SEPARATE
/// `maximumValueFinal` field, dump.cs field order), which is why it read 0 for every support
/// live even with a non-zero CurrentValue -- this is the fix, not yet re-verified live this
/// session beyond the dump.cs field-layout evidence (see DATA-CONTRACT.md "Machines").
MachineSummary Reader::ReadMachines(u64 main_base) const {
    MachineSummary out;
    const auto statics = ResolveRoot(main_base, SlotMachineModule, "MachineModule", 4);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto list_ptr = U64(*self + 0x38); // playerMachines
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    const auto machines = ListItems(*list_ptr, 256);
    out.ok = true;
    for (const u64 mp : machines) {
        if (!Ok(mp))
            continue;
        if (const auto status_ptr = U64(mp + 0x28); status_ptr && Ok(*status_ptr)) {
            std::uint8_t is_producing{};
            if (Read(*status_ptr + 0x11, &is_producing, 1) && is_producing != 0)
                ++out.working;
        }
        if (const auto output_ptr = U64(mp + 0x30); output_ptr && Ok(*output_ptr)) {
            const auto items_list = U64(*output_ptr + 0x18); // MachineOutput.items
            if (items_list && Ok(*items_list)) {
                if (const auto size = I32(*items_list + 0x18); size && *size > 0)
                    ++out.finished;
            }
        }
        const auto supports_ptr = U64(mp + 0x48); // MachineSupport[] (raw Array<T>, not List<T>)
        if (!supports_ptr || !Ok(*supports_ptr))
            continue;
        const auto length = I32(*supports_ptr + 0x18);
        if (!length || *length <= 0 || *length > 16)
            continue;
        std::vector<u64> raw(static_cast<std::size_t>(*length));
        if (!Read(*supports_ptr + 0x20, raw.data(), raw.size() * 8))
            continue;
        for (const u64 sp : raw) {
            if (!Ok(sp))
                continue;
            std::string sname;
            if (const auto sklass = U64(sp); sklass && Ok(*sklass))
                sname = ClassName(*sklass);
            const auto cur = F32(sp + 0x20);          // CurrentValue
            const auto max_final = F32(sp + 0x1C);    // maximumValueFinal
            if (!cur || !max_final || *max_final <= 0.0f)
                continue; // can't judge "lacking" without a sane max
            const bool lacking = *cur <= 0.0f;
            if (!lacking)
                continue;
            if (sname == "MachineFuelSupport")
                ++out.fuel_lack;
            else if (sname == "MachineWaterSupport")
                ++out.water_lack;
        }
    }
    return out;
}

/// Birthdays of met NPCs (`social.npcs`, already gated on mKnownList) within the next 7 days
/// (DaysUntilBirthday in [0,6]), reusing the exact same `today`/math Refresh() already derived for
/// `social.npcs[].days_until_birthday` -- round-2 fix: the two can no longer disagree (previously
/// each recomputed its own candidate-day loop independently; still correct, but duplicated).
/// DATA LANE round 3: `reminds` (ReadReminders' own output, already day-filtered) is merged in.
/// Festivals (ASSET-PATHS.md §5's CalendarFestivalAsset, a real 3-row table, PLUS the separate
/// built-in FestivalModule/FestivalEnum system found this round) still have no live "revealed to
/// this save"/lookahead source wired this pass -- see DATA-CONTRACT.md "Festivals" for exactly
/// what was and wasn't resolved; left out of `events` rather than shown unconditionally.
CalendarEvents Reader::ReadWeekEvents(const SocialFields& social,
                                      const std::vector<DayEvent>& reminds) const {
    CalendarEvents out;
    if (!social.ok)
        return out;
    out.ok = true;
    for (const NpcFields& npc : social.npcs) {
        if (npc.name.empty() || npc.days_until_birthday < 0 || npc.days_until_birthday > 6)
            continue; // round-2: gate on days_until_birthday, NOT birthday_ok (see NpcFields'
                      // own comment -- birthday_ok is true for ~every met NPC with a known
                      // birthday, not just the ones in the next 7 days; this was the task's own
                      // "cake on every Social card" bug)
        DayEvent ev;
        ev.kind = DayEvent::Kind::Birthday;
        ev.day_offset = npc.days_until_birthday;
        ev.label = npc.name;
        ev.icon = npc.portrait;
        out.events.push_back(std::move(ev));
    }
    for (const DayEvent& ev : reminds)
        out.events.push_back(ev);
    std::sort(out.events.begin(), out.events.end(),
             [](const DayEvent& a, const DayEvent& b) { return a.day_offset < b.day_offset; });
    return out;
}

/// SendGiftModule.sentNpcId (self+0x20, List<int>, `ArchivableModule<SendGiftModule>` -- 2 hops,
/// same pattern as GuildRankingManager/CookingModule/BluePrintModule). The task's own "gifted
/// today" root, found by disassembly-adjacent dump.cs reading (SendGiftModule.OnDayBegin() clears
/// it; TodayCanSendGift(Npc)/AddSendCount(int) are its own read/write pair) -- see
/// DATA-CONTRACT.md "Chat/Gift".
std::optional<std::vector<s32>> Reader::ReadGiftedToday(u64 main_base) const {
    const auto statics = ResolveRoot(main_base, SlotSendGiftModule, "SendGiftModule", 2);
    if (!statics)
        return std::nullopt;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return std::nullopt;
    const auto list_ptr = U64(*self + 0x20); // sentNpcId
    if (!list_ptr || !Ok(*list_ptr))
        return std::nullopt;
    // Bug #2 fix: share ReadChattedToday's own strict walk instead of ListInt32s, which collapses
    // every internal failure into a silently-empty vector -- that made this reader (alone, among
    // the two structurally-identical "daily action tracker" readers) unable to tell "nobody
    // gifted today" from "the list was genuinely unreadable this sample".
    return ListInt32sStrict(*list_ptr, 256);
}

// Native RelationshipPreferencePartControl.Fresh (0x1D62480..0x1D625E8) prefers
// the revealed range record, then the NPC record. Neither is the designer preference table.
void Reader::ReadKnownLikes(u64 main_base, SocialFields& social) const {
    const auto statics = ResolveRoot(main_base, SlotSendGiftModule, "SendGiftModule", 2);
    const auto self = statics ? U64(*statics) : std::nullopt;
    if (!self || !Ok(*self))
        return;
    const auto npc_list = U64(*self + 0x28);
    const auto range_list = U64(*self + 0x38);
    const auto npcs = npc_list ? ListItems(*npc_list, 256) : std::vector<u64>{};
    const auto ranges = range_list ? ListItems(*range_list, 256) : std::vector<u64>{};
    for (auto& npc : social.npcs) {
        u64 record = 0;
        bool range_record = false;
        for (const auto ptr : ranges) {
            const auto first = Ok(ptr) ? I32(ptr + 0x10) : std::nullopt;
            const auto last = Ok(ptr) ? I32(ptr + 0x14) : std::nullopt;
            if (first && last && *first <= npc.npc_id && npc.npc_id <= *last) {
                record = ptr;
                range_record = true;
                break;
            }
        }
        if (!record) {
            for (const auto ptr : npcs) {
                const auto id = Ok(ptr) ? I32(ptr + 0x10) : std::nullopt;
                if (id && *id == npc.npc_id) {
                    record = ptr;
                    break;
                }
            }
        }
        if (!record)
            continue;
        std::vector<s32> discovered;
        bool readable = true;
        std::vector<s32> excellent;
        bool first_tier = true;
        for (const u64 offset : {range_record ? 0x20ULL : 0x28ULL,
                                 range_record ? 0x28ULL : 0x30ULL}) {
            const auto list = U64(record + offset);
            const auto ids = list ? ListInt32sStrict(*list, 4096) : std::nullopt;
            if (!ids) {
                readable = false;
                break;
            }
            if (first_tier) excellent = *ids;
            first_tier = false;
            for (const s32 id : *ids) {
                if (id > 0 && std::find(discovered.begin(), discovered.end(), id) == discovered.end())
                    discovered.push_back(id);
            }
        }
        if (readable) {
            npc.known_likes = std::move(discovered);
            npc.excellent_likes = std::move(excellent);
        }
    }
}

std::optional<std::vector<s32>> Reader::ReadChattedToday(u64 main_base) const {
    const auto statics = ResolveRoot(main_base, SlotConversationManager, "ConversationManager", 3);
    if (!statics)
        return std::nullopt;
    const auto self = U64(*statics);
    if (!self || !Ok(*self))
        return std::nullopt;
    const auto list = U64(*self + 0x38); // talkedNpcId
    if (!list || !Ok(*list))
        return std::nullopt;
    return ListInt32sStrict(*list, 256);
}

/// Round-4 fix: bag + storage, not bag-only (DATA-CONTRACT.md "have counts" -- the UI lane found
/// every recipe/blueprint reading have=0/can-afford=false in a save whose ingredients happen to
/// sit entirely in storage boxes; see DATA-CONTRACT.md for the verification that this matches the
/// game's own treatment). Pure: any item_id present in either source is summed.
std::unordered_map<s32, s32> MergeHaveCounts(const InventoryFields& inventory,
                                             const StorageFields& storage) {
    std::unordered_map<s32, s32> out;
    if (inventory.ok)
        for (const InvItem& it : inventory.items)
            out[it.item_id] += it.count;
    if (storage.ok)
        for (const StorageBox& box : storage.boxes)
            for (const InvItem& it : box.items)
                out[it.item_id] += it.count;
    return out;
}

void MergeStorageItems(InventoryFields& inventory, const StorageFields& storage) {
    inventory.storage_ok = storage.ok;
    if (!storage.ok)
        return;
    // (item_id, grade) -> index into inventory.items, for the entries this call itself appends
    // (in_storage is always true for every one of them, so it adds nothing to the key here).
    std::unordered_map<std::uint64_t, std::size_t> by_id_grade;
    for (const StorageBox& box : storage.boxes) {
        for (const InvItem& it : box.items) {
            const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(it.item_id)) << 32) |
                             static_cast<std::uint32_t>(it.grade);
            const auto found = by_id_grade.find(key);
            if (found != by_id_grade.end()) {
                inventory.items[found->second].count += it.count;
            } else {
                by_id_grade.emplace(key, inventory.items.size());
                inventory.items.push_back(it); // in_storage already true, ReadItemContainerSlots
            }
        }
    }
}

/// CookingModule.unlockedCookingFormulaIds (self+0x38, List<int>, 2-hop ArchivableModule<T>)
/// joined against the static Cooking+CookingFormula tables via mtas::GameText().RecipeByOutItem
/// -- but that function keys by OUTPUT item id, not formula id, so this walks the static Cooking
/// table once to build formula_id -> out_item_id, matching every learned formula id to its dish.
IngredientsFields Reader::ReadIngredients(u64 main_base, const InventoryFields& inventory,
                                          const StorageFields& storage) const {
    IngredientsFields out;
    const auto statics = ResolveRoot(main_base, SlotCookingModule, "CookingModule", 2);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto list_ptr = U64(*self + 0x38); // unlockedCookingFormulaIds
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    out.ok = true;
    const auto formula_ids = ListInt32s(*list_ptr, 512);
    // Bag + storage "have" counts, by item id (any grade summed -- matches the Ingredients page's
    // own "can I cook this" question, which doesn't care about grade). Round 4: was bag-only.
    const std::unordered_map<s32, s32> have_by_item = MergeHaveCounts(inventory, storage);
    for (const s32 formula_id : formula_ids) {
        const RecipeConfigRow row = GameText().RecipeByFormulaId(formula_id);
        if (row.out_item_id == 0)
            continue; // no Cooking row joins to this formula id: not guessed
        RecipeFields rf;
        rf.dish_name = GameText().ItemName(row.out_item_id);
        rf.dish_icon = GameText().ItemIcon(row.out_item_id);
        rf.can_cook_now = !row.materials.empty();
        for (const RecipeMaterial& mat : row.materials) {
            MaterialNeed need;
            need.item_id = mat.item_id;
            need.item_name = GameText().ItemName(mat.item_id);
            need.item_icon = GameText().ItemIcon(mat.item_id);
            need.need = mat.count;
            const auto hit = have_by_item.find(mat.item_id);
            need.have = hit != have_by_item.end() ? hit->second : 0;
            need.can_afford = need.have >= need.need;
            if (!need.can_afford)
                rf.can_cook_now = false;
            rf.materials.push_back(std::move(need));
        }
        out.known.push_back(std::move(rf));
    }
    return out;
}

/// BluePrintModule.unlockedBluePrintDatas (self+0x28, List<PrintUnlockData>, VALUE-type struct,
/// stride 0x18: itemId:i32@0, unlockTime:GameDateTime(u64)@8, isFresh:bool@0x10) -- the unlocked
/// ITEM id directly, joined against Creation/CreationPart via mtas::GameText().AssemblyRecipeFor.
HandbookFields Reader::ReadHandbook(u64 main_base, const InventoryFields& inventory,
                                    const StorageFields& storage) const {
    HandbookFields out;
    const auto statics = ResolveRoot(main_base, SlotBluePrintModule, "BluePrintModule", 2);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto list_ptr = U64(*self + 0x28); // unlockedBluePrintDatas
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    out.ok = true;
    const auto items_ptr = U64(*list_ptr + 0x10);
    const auto size = I32(*list_ptr + 0x18);
    if (!items_ptr || !Ok(*items_ptr) || !size || *size < 0 || *size > 65536)
        return out;
    const auto capacity = I32(*items_ptr + 0x18);
    if (!capacity || *capacity < *size || *capacity > 65536)
        return out;
    constexpr std::size_t Stride = 0x18;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(*size) * Stride);
    if (!raw.empty() && !Read(*items_ptr + 0x20, raw.data(), raw.size()))
        return out;
    std::unordered_map<s32, bool> unlocked;
    std::vector<s32> unlocked_ids;
    for (std::size_t i = 0; i < static_cast<std::size_t>(*size); ++i) {
        s32 item_id{};
        std::memcpy(&item_id, raw.data() + i * Stride, 4);
        if (item_id > 0) {
            unlocked[item_id] = raw[i * Stride + 0x10] != 0;
            unlocked_ids.push_back(item_id);
        }
    }
    const auto have_by_item = MergeHaveCounts(inventory, storage);
    for (const auto& row : GameText().HandbookCatalogueFor(unlocked_ids)) {
        BlueprintFields bf;
        bf.category = row.category;
        const auto hit = unlocked.find(row.item_id);
        bf.unlocked = hit != unlocked.end();
        if (bf.unlocked) {
            bf.item_id = row.item_id;
            bf.item_name = GameText().ItemName(row.item_id);
            bf.item_icon = GameText().ItemIcon(row.item_id);
            bf.fresh = hit->second;
            const auto recipe = GameText().AssemblyRecipeFor(row.item_id);
            for (const auto& material : recipe.materials) {
                MaterialNeed need;
                need.item_id = material.item_id;
                need.item_name = GameText().ItemName(material.item_id);
                need.item_icon = GameText().ItemIcon(material.item_id);
                need.need = material.count;
                const auto have = have_by_item.find(material.item_id);
                need.have = have != have_by_item.end() ? have->second : 0;
                need.can_afford = need.have >= need.need;
                bf.materials.push_back(std::move(need));
            }
        }
        out.slots.push_back(std::move(bf));
    }
    return out;
}

/// CalendarRemindModule.reminds (self+0x20, List<RemindSingle>, reference-type elements,
/// 2-hop ArchivableModule<T>). RemindSingle: Year/Month/Day (self+0x18/0x1C/0x20); its base class
/// RemindBasic.remindItems (self+0x10, List<RemindItem>) -- first item only, matching the native
/// Calendar's own one-line-per-day reminder display. RemindItem: Hour/Minute (self+0x10/0x14),
/// RemindStr chooses presetIndex first, then DefaultTransId, then customString. The custom
/// string can retain an earlier label even while a preset is selected.
std::vector<DayEvent> Reader::ReadReminders(u64 main_base, u64 today_day_of_world) const {
    std::vector<DayEvent> out;
    const auto statics = ResolveRoot(main_base, SlotCalendarRemindModule, "CalendarRemindModule", 2);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto reminds_ptr = U64(*self + 0x20); // reminds
    if (!reminds_ptr || !Ok(*reminds_ptr))
        return out;
    for (const u64 rp : ListItems(*reminds_ptr, 64)) {
        if (!Ok(rp))
            continue;
        const auto year = I32(rp + 0x18);
        const auto month = I32(rp + 0x1C);
        const auto day = I32(rp + 0x20);
        if (!year || !month || !day || *month < 1 || *month > 4 || *day < 1 || *day > 28)
            continue;
        // Same day_of_world basis CalendarFromDayOfWorld uses: (year-1)*112 + (month-1)*28 +
        // (day-1), 0-based.
        const u64 candidate = static_cast<u64>(*year - 1) * 112 + static_cast<u64>(*month - 1) * 28 +
                              static_cast<u64>(*day - 1);
        if (candidate < today_day_of_world || candidate - today_day_of_world > 6)
            continue; // outside the 7-day window (also drops stale past reminders)
        // A RemindSingle exists for every day the module has ever touched (verified live this
        // round: "today"'s own entry was present with remindItems EMPTY in a fixture where the
        // native Calendar shows no reminder icon on that day at all) -- only a NON-EMPTY
        // remindItems list is a real, player-added reminder; an empty one is not guessed into a
        // blank-label event.
        const auto item_list = U64(rp + 0x10);
        if (!item_list || !Ok(*item_list))
            continue;
        const auto item_ptrs = ListItems(*item_list, 16);
        if (item_ptrs.empty() || !Ok(item_ptrs.front()))
            continue;
        const u64 item = item_ptrs.front();
        const auto preset = I32(item + 0x20);
        const auto default_id = I32(item + 0x24);
        std::string label;
        if (preset && *preset >= 0) {
            if (const auto fields = ResolveRoot(main_base, SlotCalendarRemindPresetHolder,
                                                "CalendarRemindPresetHolder", 0)) {
                if (const auto list = U64(*fields); list && Ok(*list)) {
                    const auto ids = ListInt32s(*list, 64);
                    if (!ids.empty())
                        label = GameText().Get(ids[std::min<std::size_t>(*preset, ids.size() - 1)]);
                }
            }
        } else if (default_id && *default_id > 0) {
            label = GameText().Get(*default_id);
        } else if (const auto str_ptr = U64(item + 0x18); str_ptr && Ok(*str_ptr)) {
            label = DotNetString(*str_ptr, 256);
        }
        DayEvent ev;
        ev.kind = DayEvent::Kind::Reminder;
        ev.day_offset = static_cast<s32>(candidate - today_day_of_world);
        ev.label = label;
        out.push_back(std::move(ev));
    }
    return out;
}

/// FestivalModule.festivalConfigs (self+0xA8, List<FestivalConfig>). See mtas_reader.h's own
/// comment on SlotFestivalModule/ReadFestivals for the full live-verification evidence.
std::vector<DayEvent> Reader::ReadFestivals(u64 main_base, u64 today_day_of_world) const {
    std::vector<DayEvent> out;
    const auto statics = ResolveRoot(main_base, SlotFestivalModule, "FestivalModule", 3);
    if (!statics)
        return out;
    const auto self = U64(*statics + 0x0);
    if (!self || !Ok(*self))
        return out;
    const auto configs_ptr = U64(*self + 0xA8); // festivalConfigs
    if (!configs_ptr || !Ok(*configs_ptr))
        return out;
    for (const u64 cp : ListItems(*configs_ptr, 32)) {
        if (!Ok(cp))
            continue;
        std::uint8_t enable_byte{};
        if (!Read(cp + 0x10, &enable_byte, 1) || enable_byte == 0)
            continue; // the reveal gate: skip any festival not marked enabled
        const auto festival_enum = I32(cp + 0x14);
        const auto start_month = I32(cp + 0x24);
        const auto start_day = I32(cp + 0x28);
        const auto end_month = I32(cp + 0x30);
        const auto end_day = I32(cp + 0x34);
        if (!festival_enum || !start_month || !start_day || !end_month || !end_day)
            continue;
        const s32 start_offset = DaysUntilBirthday(today_day_of_world, *start_month, *start_day);
        if (start_offset < 0)
            continue; // malformed month/day, not guessed
        const u64 start_day_of_world = today_day_of_world + static_cast<u64>(start_offset);
        const s32 day_of_year_start = (*start_month - 1) * 28 + (*start_day - 1);
        const s32 day_of_year_end = (*end_month - 1) * 28 + (*end_day - 1);
        s32 duration = day_of_year_end - day_of_year_start;
        if (duration < 0 || duration > 10)
            duration = 0; // defensive clamp; every observed festival spans 0-4 days same-year
        // FestivalModule.GetFestivalName (main+0x398BFE0) uses festivalProto[enum].nameId.
        std::string label;
        const auto protos = U64(*self + 0x40);
        const auto count = protos && Ok(*protos) ? I32(*protos + 0x18) : std::nullopt;
        if (count && *count > 0 && *count <= 32 && *festival_enum >= 0 &&
            *festival_enum < *count) {
            const auto proto = U64(*protos + 0x20 + static_cast<u64>(*festival_enum) * 8);
            if (proto && Ok(*proto)) {
                if (const auto name_id = I32(*proto + 0x18))
                    label = GameText().Get(*name_id);
            }
        }
        for (s32 d = 0; d <= duration; ++d) {
            const u64 day_of_world = start_day_of_world + static_cast<u64>(d);
            if (day_of_world < today_day_of_world || day_of_world - today_day_of_world > 6)
                continue;
            DayEvent ev;
            ev.kind = DayEvent::Kind::Festival;
            ev.day_offset = static_cast<s32>(day_of_world - today_day_of_world);
            ev.label = label;
            out.push_back(std::move(ev));
        }
    }
    return out;
}

/// Unit<StorageUnit> (0-hop closed-generic root) -> static_fields -> `unitList` (List<StorageUnit>,
/// scene-scoped). Live-verified this round (tools/probe_round3.py): `unitList` IS the first
/// static field (static_fields+0x0) -- cross-checked two ways, not just guessed from declaration
/// order: (1) the SECOND static field, `<UnitList>k__BackingField` (a `ReadOnlyCollection<T>`),
/// itself points to an object whose own +0x10 field equals this exact pointer (the ctor's own
/// `UnitList = new ReadOnlyCollection<T>(unitList)`, dump.cs); (2) every resolved `StorageUnit`
/// instance's own class name read back `"StorageUnit"` and its `Storage` slot list gave sane,
/// non-degenerate counts (DATA-CONTRACT.md "Storage" has the full 4-box live result).
StorageFields Reader::ReadStorage(u64 main_base) const {
    StorageFields out;
    const auto klass = U64(main_base + SlotUnitStorageUnit);
    if (!klass || !Ok(*klass))
        return out;
    const auto statics = U64(*klass + OffStaticFields);
    if (!statics || !Ok(*statics))
        return out;
    const auto list_ptr = U64(*statics + 0x0); // unitList, live-verified above
    if (!list_ptr || !Ok(*list_ptr))
        return out;
    out.ok = true;
    for (const u64 unit_ptr : ListItems(*list_ptr, 64)) {
        if (!Ok(unit_ptr))
            continue;
        StorageBox box;
        if (const auto name_ptr = U64(unit_ptr + 0xD0); name_ptr && Ok(*name_ptr))
            box.custom_name = DotNetString(*name_ptr, 64);
        if (const auto storage_ptr = U64(unit_ptr + 0xC0); storage_ptr && Ok(*storage_ptr)) {
            // Round-4 addition: per-item breakdown (id/name/icon/count/grade), same path
            // ReadInventory's own bag walk uses -- feeds MergeHaveCounts.
            box.items = ReadItemContainerSlots(*storage_ptr, /*in_storage=*/true);
            box.item_count = static_cast<s32>(box.items.size());
        }
        out.boxes.push_back(std::move(box));
    }
    return out;
}

std::optional<std::array<s32, 4>> Reader::ReadGradeNameIds(u64 main_base) const {
    const auto statics = ResolveRoot(main_base, SlotGlobalModule, "GlobalModule", 1);
    const auto self = statics ? U64(*statics) : std::nullopt;
    const auto misc = self && Ok(*self) ? U64(*self + 0x18) : std::nullopt;
    const auto names = misc && Ok(*misc) ? U64(*misc + 0x98) : std::nullopt;
    std::array<s32, 4> ids{};
    if (!names || !Ok(*names) || !Read(*names + 0x10, ids.data(), sizeof ids))
        return std::nullopt;
    for (const s32 id : ids)
        if (id <= 0)
            return std::nullopt;
    return ids;
}

MapFields Reader::ReadMap(const EdenDsmodHostApi& host, bool with_history) {
    host_ = &host;
    MapFields out;
    const auto statics = ResolveRoot(host.main_base, SlotMapModule, "MapModule", 3);
    const auto self = statics ? U64(*statics) : std::nullopt;
    if (!self || !Ok(*self))
        return out;
    const auto scene = I32(*self + 0x58);
    const auto history = U64(*self + 0xA0);
    if (!scene || !history || !Ok(*history))
        return out;
    out.ok = true;
    out.self = *self;
    out.history = *history;
    out.scene = *scene;
    if (const auto cfg = U64(*self + 0x60); cfg && Ok(*cfg)) {
        const auto name_id = I32(*cfg + 0x10);
        const auto cfg_scene = I32(*cfg + 0x14);
        if (name_id && cfg_scene && *cfg_scene == *scene && *name_id > 0)
            out.name_id = *name_id;
    }
    if (const auto cfg = U64(*self + 0x60); cfg && Ok(*cfg)) {
        float geometry[3]{};
        if (Read(*cfg + 0x40, geometry, sizeof geometry) && std::isfinite(geometry[0]) &&
            std::isfinite(geometry[1]) && std::isfinite(geometry[2]) && geometry[2] > 0) {
            out.center_x = geometry[0]; out.center_z = geometry[1]; out.radius = geometry[2];
        }
    }
    // Actor pose is independent of the fog-stamp cadence (MapModule.coordinate).
    const auto player_statics = ResolveRoot(host.main_base, SlotPlayer, "Player", 3);
    const auto player = player_statics ? U64(*player_statics) : std::nullopt;
    const auto actor = player && Ok(*player) ? U64(*player + 0x38) : std::nullopt;
    const auto trans = actor && Ok(*actor) ? U64(*actor + 0x28) : std::nullopt;
    if (trans && Ok(*trans) && I32(*trans + 0x74) == out.scene && out.radius > 0) {
        float position[3]{}, forward[3]{};
        if (Read(*trans + 0x10, position, sizeof position) &&
            std::all_of(std::begin(position), std::end(position),
                [](float v) { return std::isfinite(v) && std::abs(v) < 100000; })) {
            out.actor_u = 0.5f + (position[0] - out.center_x) / (2 * out.radius);
            out.actor_v = 0.5f + (position[2] - out.center_z) / (2 * out.radius);
            out.actor_ok = out.actor_u >= 0 && out.actor_u <= 1 && out.actor_v >= 0 && out.actor_v <= 1;
        }
        if (Read(*trans + 0x2C, forward, sizeof forward) &&
            std::isfinite(forward[0]) && std::isfinite(forward[2]) &&
            std::hypot(forward[0], forward[2]) > 0.1f) {
            out.facing_yaw = std::atan2(forward[0], forward[2]) * 180.0f / 3.14159265358979323846f;
            out.facing_ok = true;
        }
    }
    // Reuse the native UI's per-instance map data, including its computed reveal gates.
    // The Gaming state remains in stateList while MainMenu is stacked above it.
    const auto klass = U64(host.main_base + SlotUiSystemMgr);
    const auto ui_statics = klass && Ok(*klass) && ClassName(*klass) == "UISystemMgr" ? U64(*klass + OffStaticFields) : std::nullopt;
    const auto instance = ui_statics && Ok(*ui_statics) ? U64(*ui_statics + 0x28) : std::nullopt;
    const auto layer = instance && Ok(*instance) ? U64(*instance + 0xB0) : std::nullopt;
    const auto states = layer && Ok(*layer) ? U64(*layer + 0x38) : std::nullopt;
    u64 mini{}, full{};
    if (states && Ok(*states)) {
        for (const auto state : ListItems(*states, 64)) {
            const auto control = U64(state + 0x58); // UIAssetBase.control
            const auto type = control && Ok(*control) ? U64(*control) : std::nullopt;
            if (!type || !Ok(*type)) continue;
            const auto name = ClassName(*type);
            if (name == "GamingControl") mini = U64(*control + 0x40).value_or(0);
            if (name == "MainMenuControl") {
                const auto map_control = U64(*control + 0x48);
                if (map_control && U64(*control + 0x90) == map_control) full = *map_control;
            }
        }
    }
    // Minimap pose/scale also remain available while the full native map is open.
    if (Ok(mini)) {
        const auto control = mini;
        {
            const auto ui = U64(control + 0x10);
            const auto viewer = ui && Ok(*ui) ? U64(*ui + 0x30) : std::nullopt;
            if (viewer && Ok(*viewer)) {
                const auto cfg = U64(control + 0x78);
                const auto size = F32(*viewer + 0x64);
                const auto scale = cfg && Ok(*cfg) ? F32(*cfg + 0x68) : std::nullopt;
                const auto mask = F32(*ui + 0x58);
                if (size && scale && mask && *size > 0 && *scale > 0 && *mask > 0) {
                    out.minimap_size = *size;
                    out.minimap_scale = *scale;
                    out.minimap_mask_radius = std::sqrt(*mask);
                }
            }
        }
        const auto dict = U64(mini + 0x38);
        if (dict && Ok(*dict)) {
            for (const auto& [unused_key, info] : DictEntries(*dict, 4096)) {
                if (!Ok(info)) continue;
                const auto type = U64(info);
                if (!type || !Ok(*type) || ClassName(*type) != "MapIconInfo") continue;
                const auto icon = U64(info + 0x88);
                if (!icon || !Ok(*icon)) continue;
                auto sprite = DotNetString(*icon, 240);
                const auto slash = sprite.find_last_of('/');
                if (slash != std::string::npos) sprite.erase(0, slash + 1);
                // CameraModule.IMap.GetGameRot reads mainCamera.transform.eulerAngles.
                if (sprite != "I_Main_img_MapView_0" || I32(info + 0x14) != out.scene) continue;
                const auto yaw = F32(info + 0x2C);
                if (yaw && std::isfinite(*yaw)) {
                    out.camera_yaw = *yaw;
                    out.camera_ok = true;
                    out.camera_sprite = std::move(sprite);
                }
            }
        }
    }
    // Prefer the big map while it is present; do not cache a closed UI's markers.
    for (const auto control : {full, mini}) {
        const auto type = Ok(control) ? U64(control) : std::nullopt;
        if (!type || !Ok(*type)) continue;
        const auto name = ClassName(*type);
        const bool big = name == "MapPartControl";
        if (!big && name != "MiniMapPartControl") continue;
        if (I32(control + (big ? 0x50 : 0x40)) != out.scene) continue;
        const auto dict = U64(control + 0x38);
        if (!dict || !Ok(*dict)) continue;
        std::unordered_set<u64> displayed;
        if (big) {
            // MapPartControl does not update minimap hasDisplay flags. Membership of the
            // native MapViewer's display pool is its CanShow/CheckDisplay result instead.
            const auto ui = U64(control + 0x10);
            const auto viewer = ui && Ok(*ui) ? U64(*ui + 0x30) : std::nullopt;
            const auto pool = viewer && Ok(*viewer) ? U64(*viewer + 0x30) : std::nullopt;
            const auto pool_dict = pool && Ok(*pool) ? U64(*pool + 0x20) : std::nullopt;
            const auto entries = pool_dict && Ok(*pool_dict) ? U64(*pool_dict + 0x18) : std::nullopt;
            const auto count = pool_dict && Ok(*pool_dict) ? I32(*pool_dict + 0x20) : std::nullopt;
            if (!entries || !Ok(*entries) || !count || *count < 0 || *count > 4096) continue;
            if (I32(*entries + 0x18).value_or(-1) < *count) continue;
            for (int i = 0; i < *count; ++i) {
                const auto entry = *entries + 0x20 + static_cast<u64>(i) * 24;
                if (I32(entry).value_or(-1) < 0) continue;
                const auto info = U64(entry + 8), view = U64(entry + 16);
                const auto view_class = view && Ok(*view) ? U64(*view) : std::nullopt;
                if (info && Ok(*info) && view_class && Ok(*view_class) &&
                    ClassName(*view_class) == "MapIconUI" && I32(*view + 0x184) == 0)
                    displayed.insert(*info);
            }
        }
        for (const auto& [unused_key, info] : DictEntries(*dict, 4096)) {
            if (!Ok(info)) continue;
            const auto info_type = U64(info);
            if (!info_type || !Ok(*info_type) || ClassName(*info_type) != "MapIconInfo") continue;
            std::uint8_t flags[3]{};
            // isFullyClosed, hasDisplay, requireDisplay; positive native visibility only.
            if (big ? !displayed.contains(info) : (!Read(info + 0xE2, flags, sizeof flags) || flags[0] != 0 || flags[1] != 1 || flags[2] != 1)) continue;
            if (I32(info + 0x14) != out.scene) continue;
            const auto icon = U64(info + 0x88);
            if (!icon || !Ok(*icon)) continue;
            auto sprite = DotNetString(*icon, 240);
            const auto slash = sprite.find_last_of('/');
            if (slash != std::string::npos) sprite.erase(0, slash + 1);
            if ((!sprite.starts_with("I_Map_") && !sprite.starts_with("MapIcon_Mission") && !sprite.ends_with("_Mini")) || sprite == "I_Map_icon_Player_00") continue;
            float position[3]{}, size[2]{};
            if (!Read(info + 0x1C, position, sizeof position) || !Read(info + 0x34, size, sizeof size)) continue;
            if (!std::all_of(std::begin(position), std::end(position), [](float v) { return std::isfinite(v) && std::abs(v) < 100000; })) continue;
            std::uint8_t in_room{};
            if (!Read(info + 0x50, &in_room, 1) || in_room > 1) continue;
            std::string background;
            if (I32(info + 0xD4) == 1) { // native Monster background, not an inferred color
                const auto bg = U64(info + 0xD8);
                if (bg && Ok(*bg)) background = DotNetString(*bg, 240);
                if (!background.starts_with("I_Map_img_MonsterBg_")) continue;
            }
            out.markers.push_back({sprite, position[0], position[1], position[2], size[0], size[1], in_room != 0, background});
        }
        out.markers_ok = true;
        if (!out.markers.empty()) break;
    }
    float coord[2]{};
    if (Read(*self + 0x88, coord, sizeof coord) && std::isfinite(coord[0]) &&
        std::isfinite(coord[1]) && coord[0] >= 0.0f && coord[0] < 1.0f && coord[1] >= 0.0f &&
        coord[1] < 1.0f) {
        out.coord_ok = true;
        out.u = coord[0];
        out.v = coord[1];
    }
    if (const auto archive_statics = ResolveRoot(host.main_base, SlotArchiveMgr, "ArchiveMgr", 1)) {
        if (const auto archive = U64(*archive_statics); archive && Ok(*archive)) {
            if (const auto name = U64(*archive + 0x70); name && Ok(*name))
                out.save_name = DotNetString(*name, 200);
        }
    }
    if (with_history && I32(*history + 0x18) == 256) {
        std::vector<u64> rows(256);
        out.history_bits.assign(256 * 32, 0);
        bool ok = Read(*history + 0x20, rows.data(), rows.size() * sizeof(u64));
        for (std::size_t x = 0; ok && x < rows.size(); ++x) {
            const auto words = Ok(rows[x]) && I32(rows[x] + 0x18) == 256 ? U64(rows[x] + 0x10)
                                                                         : std::nullopt;
            ok = words && Ok(*words) && I32(*words + 0x18) == 8 &&
                 Read(*words + 0x20, out.history_bits.data() + x * 32, 32);
        }
        out.history_ok = ok;
        if (!ok)
            out.history_bits.clear();
    }
    return out;
}

std::optional<bool> Reader::ReadLoading(const EdenDsmodHostApi& host) {
    host_ = &host;
    const auto statics = ResolveRoot(host.main_base, SlotScenarioModule, "ScenarioModule", 2);
    if (!statics)
        return std::nullopt;
    const auto self = U64(*statics);
    if (!self || !Ok(*self))
        return std::nullopt;
    std::uint8_t posting{};
    if (!Read(*self + 0xB1, &posting, 1) || posting > 1)
        return std::nullopt;
    if (posting)
        return true;
    // PostingLoading clears when closing begins. playend stays set until CloseDisplay
    // disables the mask root (main+0x2E98418..0x2E9845C), covering that final tween.
    const auto klass = U64(host.main_base + SlotUiSystemMgr);
    if (!klass || !Ok(*klass) || ClassName(*klass) != "UISystemMgr")
        return std::nullopt;
    const auto ui_statics = U64(*klass + OffStaticFields);
    const auto instance = ui_statics && Ok(*ui_statics) ? U64(*ui_statics + 0x28) : std::nullopt;
    const auto shared = instance && Ok(*instance) ? U64(*instance + 0x18) : std::nullopt;
    const auto dict = shared && Ok(*shared) ? U64(*shared + 0x28) : std::nullopt;
    if (!dict || !Ok(*dict))
        return std::nullopt;
    for (const auto& [kind, asset] : DictEntries(*dict, 256)) {
        if (kind != 8 || !Ok(asset)) // UISharedOnScreenType.LoadingMask
            continue;
        const auto view = U64(asset + 0x50);
        const auto view_class = view && Ok(*view) ? U64(*view) : std::nullopt;
        if (!view_class || ClassName(*view_class) != "LoadingMaskUI")
            return std::nullopt;
        std::uint8_t closing{};
        if (!Read(*view + 0x120, &closing, 1) || closing > 1)
            return std::nullopt;
        return closing != 0;
    }
    return std::nullopt;
}

Model Reader::Refresh(const EdenDsmodHostApi& host) {
    host_ = &host;
    // The package point uses the shared IL2CPP name resolver for OptionMgr, then
    // Singleton.instance -> languageGame (OptionSlot) -> valueInt (dump.cs 3835/22969).
    const auto language = host.get_i64 ? host.get_i64(host.userdata, "game.language", -1) : -1;
    if (language >= 0 && language < 13)
        language_ = static_cast<s32>(language);
    GameText().Load(host, language_);
    Model m;
    const u64 main_base = host.main_base;
    m.time = ReadTime(main_base);
    m.bag = ReadBag(main_base);
    m.ui = ReadUi(main_base);
    m.dialogue = ReadDialogueSpeaker(main_base); // Round 8: independent of game_loaded, same as
                                                 // ui itself (the title screen's own menus are
                                                 // UI pushes too, just never class "DialogUI")
    m.game_loaded = m.bag.ok; // Player only resolves to a live instance once a save is loaded
    if (m.game_loaded && !was_game_loaded_) {
        // PERF defensive invalidation: a fresh load (title -> gameplay, possibly a different save
        // or build than whatever was last cached) drops every root/index cache -- see this
        // member's own header comment for why this is insurance, not a load-bearing fix.
        root_cache_.clear();
        mission_base_info_dict_ = 0;
        mission_base_info_count_ = -1;
        mission_base_info_cache_.clear();
    }
    was_game_loaded_ = m.game_loaded;
    u64 current_date_data = 0;
    bool have_clock = false;
    if (m.time.ok) {
        // Re-read CurrentTime.dateData once more here (ReadTime already validated the chain);
        // cheap, and keeps ReadMissions a pure function of a passed-in tick value for testing.
        if (const auto statics = ResolveRoot(main_base, SlotTimeModule, "TimeModule", 3)) {
            if (const auto self = U64(*statics + 0x0); self && Ok(*self)) {
                if (const auto dd = U64(*self + 0x50)) {
                    current_date_data = *dd;
                    have_clock = true;
                }
            }
        }
    }
    m.missions = ReadMissions(main_base, current_date_data, have_clock);
    m.tracked = ReadTracked(main_base, current_date_data, have_clock);
    m.social = ReadSocial(main_base);
    ReadKnownLikes(main_base, m.social);
    // DATA LANE round 3: SendGiftModule.sentNpcId merged onto social.npcs here (same pattern as
    // the birthday merge below -- ReadSocial has no reason to read SendGiftModule itself).
    if (const auto chatted_ids = ReadChattedToday(main_base)) {
        for (NpcFields& npc : m.social.npcs) {
            npc.chatted_ok = true;
            npc.chatted_today = std::find(chatted_ids->begin(), chatted_ids->end(), npc.npc_id) !=
                                chatted_ids->end();
        }
    }
    if (const auto gifted_ids = ReadGiftedToday(main_base)) {
        for (NpcFields& npc : m.social.npcs) {
            npc.gifted_ok = true; // the chain resolved (even if this npc isn't in the list)
            npc.gifted_today = std::find(gifted_ids->begin(), gifted_ids->end(), npc.npc_id) !=
                               gifted_ids->end();
        }
    } // else: chain did not resolve this sample -- every npc's gifted_ok/gifted_today stay false
      // (NpcFields' own default), not silently claimed "resolved, not gifted"
    std::vector<DayEvent> reminds;
    if (have_clock) {
        // Round-2: today's day_of_world, same mask/constant ReadTime's own calendar decode uses
        // (CalendarFromDateData). Filled onto social.npcs here (not inside ReadSocial, which has
        // no reason to read TimeModule itself) so ReadWeekEvents' 7-day gate and each NpcFields'
        // own days_until_birthday can never disagree -- they're the same value.
        constexpr u64 TicksMask = 0x3FFFFFFFFFFFFFFFULL;
        constexpr u64 TicksPerDay = 24ULL * 60 * 60 * 1000 * 10000;
        const u64 today = (current_date_data & TicksMask) / TicksPerDay;
        for (NpcFields& npc : m.social.npcs)
            npc.days_until_birthday =
                npc.birthday_ok ? DaysUntilBirthday(today, npc.birthday_month, npc.birthday_day)
                                : -1;
        reminds = ReadReminders(main_base, today); // DATA LANE round 3
        // Round-4 follow-up: festivals merged in alongside reminders (both are already-computed,
        // day-windowed DayEvent lists; ReadWeekEvents just appends+sorts).
        for (DayEvent& ev : ReadFestivals(main_base, today))
            reminds.push_back(std::move(ev));
        m.week = ReadWeekEvents(m.social, reminds);
    }
    m.inventory = ReadInventory(main_base);
    m.workshop = ReadWorkshop(main_base);
    m.player = ReadPlayer(main_base);
    m.machines = ReadMachines(main_base);
    // DATA LANE round 3 additions:
    m.storage = ReadStorage(main_base); // read before Ingredients/Handbook, round 4: both need it
    m.ingredients = ReadIngredients(main_base, m.inventory, m.storage);
    m.handbook = ReadHandbook(main_base, m.inventory, m.storage);
    // Bug fix: AFTER Ingredients/Handbook, which already sum inventory+storage counts straight
    // from these same two original, unmerged fields via MergeHaveCounts -- merging storage into
    // inventory.items first would double-count every storage item in their "have" sums.
    MergeStorageItems(m.inventory, m.storage);
    if (const auto ids = ReadGradeNameIds(main_base)) {
        m.grade_names_ok = true;
        m.grade_name_ids = *ids;
    }
    host_ = nullptr;
    return m;
}

} // namespace mtas
