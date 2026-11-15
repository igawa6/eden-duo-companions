// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// My Time at Sandrock (0100B63016916000) live reader, update v851968, main build
// b74dfd27fc9da1b4e19d06e3d40dd370 (Unity 2021.3.26f1 IL2CPP, Switch). Read-only.
//
// Root-resolution pattern (confirmed live 2026-11-07 against this build, ms_reader.h / Chained
// Echoes precedent):
//   TypeInfo slot (main-relative, il2cpp-dump/script.json) -> Il2CppClass* (name
//   verified) -> walk klass->parent (+0x58) N times to the concrete Module<T> class -> that
//   class's static_fields (+0xB8) -> +0x0 = Module<T>.self (the live instance).
// UISystemMgr is not generic: its own static_fields carries <Instance>k__BackingField directly
// at +0x28 (0 hops).
//
// This slice reads only what Stage-8 needs: TimeModule (day/hour/minute + the Month/Season/Year/
// DayOfWeek calendar decode from CurrentTime.dateData) and Player -> Bag.Gold. UISystemMgr's
// stateType is read too (cheap, same pattern) to distinguish free-roam from a menu/dialog, but is
// not gated on in this slice. Every field is guarded: a chain that does not resolve (no save
// loaded, title screen, a bad build) reports "missing", never a stale or invented value.
#pragma once

#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mtas {

using u64 = std::uint64_t;
using s32 = std::int32_t;

// TypeInfo slots, main-relative (il2cpp-dump/script.json ScriptMetadata, this build's own
// exefs/main sha256 f2c589291630c415213941cb6ea00b4b20bd523975a87333ed7152cc6d4cc00e).
// Verified live, two full boots.
inline constexpr u64 SlotTimeModule = 0x80B8328;
inline constexpr u64 SlotUiSystemMgr = 0x80B8A28;
inline constexpr u64 SlotScenarioModule = 0x80B6C20;
inline constexpr u64 SlotPlayer = 0x80B5708;
inline constexpr u64 SlotMissionManager = 0x80B4090;
// Stage-9 data-contract additions (2026-11-07). Roots verified live by the Stage-5 probe
// (research/.../analysis/LIVE-READS.md §2/§5: SocialModule, GuildRankingManager) and the
// Stage-9 fidelity round (STATUS.md §13 Fix 5, tools/probe_machines.py: MachineModule) --
// `tools/live_reads.py`/`probe_machines.py` against this same build's script.json.
inline constexpr u64 SlotSocialModule = 0x80B7640;
inline constexpr u64 SlotGuildRankingManager = 0x80B0628;
inline constexpr u64 SlotMachineModule = 0x80B3958;
// DATA LANE round 3 additions (2026-11-07). From script.json's own ScriptMetadata (same lookup
// method as every slot above -- `_TypeInfo` entry's Address, main-relative), live-verified this
// round (research/.../tools/probe_round3.py) unless a field's own comment says otherwise.
inline constexpr u64 SlotSendGiftModule = 0x80B7000;       ///< Pathea.SendGiftNs.SendGiftModule
inline constexpr u64 SlotConversationManager = 0x80AE318; ///< Pathea.ConversationNs.ConversationManager
inline constexpr u64 SlotCookingModule = 0x80AE3B8;        ///< Pathea.CookingNs.CookingModule
inline constexpr u64 SlotBluePrintModule = 0x80AD368;      ///< Pathea.BluePrintNs.BluePrintModule
inline constexpr u64 SlotCalendarRemindPresetHolder = 0x80AD888;
inline constexpr u64 SlotCalendarRemindModule = 0x80AD880; ///< Pathea.CalendarRemindModule
/// Pathea.HomeNs.Unit<StorageUnit> -- a CLOSED generic instantiation's own TypeInfo slot (not the
/// open Unit<T> template): script.json carries one `_TypeInfo` entry per generic instantiation
/// IL2CPP actually emits code for, and `Unit<StorageUnit>` is one of them (the game calls
/// `Unit<StorageUnit>.ForeachUnit`/`.UnitList` directly). Root resolution is 0 HOPS here (same
/// shape as UISystemMgr): the klass at this slot IS the generic instantiation whose own
/// static_fields holds `unitList` at offset 0x0, live-confirmed this round two independent ways
/// (mtas_reader.cpp's own comment on ReadStorage has both) -- dump.cs itself could not resolve
/// Unit<T>'s per-field offsets for this open generic, so this was a live-only finding.
inline constexpr u64 SlotUnitStorageUnit = 0x80A9DC0;
// Round-4 follow-up (2026-11-07). Pathea.FestivalNs.FestivalModule, ArchivableUpdatableModule<T>
// (3 hops, same pattern as TimeModule/SocialModule). Live-verified this round: `FestivalTime`'s
// 4 Vector3Int fields (prepare/start/end/finish, dump.cs) are `(month, day, hour)` -- confirmed
// decisively against the task's own ground truth, `WinterFestival`'s own `start=[4,28,7]`/
// `end=[4,28,23]` (month 4 = Winter, day 28 -- exactly the task's day-28-Winter capture).
inline constexpr u64 SlotFestivalModule = 0x80AFA98;
// Map page (2026-11-09). Pathea.MapNs.MapModule (ArchivableUpdatableModule<T>, 3 hops) and
// Pathea.ArchiveNs.ArchiveMgr (1 hop), both live-verified by the item 21/21c probes
// (work/item21/probe-map.py, work/item21c/read-slot.py).
inline constexpr u64 SlotMapModule = 0x80B3B90;
inline constexpr u64 SlotArchiveMgr = 0x80AC9F0;
/// Pathea.GlobalModule (Module<T>, 1 hop): GlobalMisc (self+0x18, GlobalScriptableObject) ->
/// gradeName (+0x98, ItemGradeName) -> trans0..3 (+0x10..+0x1C), the text ids the native
/// GetGradeName(GradeType) returns for Grade_0..3 (Ordinary/Outstanding/Perfect/Rare in English).
inline constexpr u64 SlotGlobalModule = 0x80B02F0;

// il2cpp.h Il2CppClass_1 (this build): name +0x10, namespace +0x18 (unused -- no call site in
// this codebase has ever compared a namespace to anything, see ClassName's own comment), parent
// +0x58; Il2CppClass.static_fields right after Il2CppClass_1, at +0xB8.
inline constexpr u64 OffName = 0x10;
inline constexpr u64 OffParent = 0x58;
inline constexpr u64 OffStaticFields = 0xB8;

struct TimeFields {
    bool weather_ok{false};
    s32 weather{0};
    bool ok{false};        ///< the whole TimeModule chain resolved
    s32 day{0};            ///< CurDay (self+0x58)
    s32 hour{0};           ///< CurHour (self+0x5C)
    s32 minute{0};         ///< CurMinute (self+0x60)
    bool calendar_ok{false};
    u64 day_of_world{0};   ///< 0-based absolute day count (season/year-safe strip math)
    s32 year{0};
    s32 month{0};          ///< 1..4 (this build's "month" is the season slot)
    std::string season;    ///< "Spring" | "Summer" | "Autumn" | "Winter"
    std::string weekday;   ///< "Sunday".."Saturday"
};

struct BagFields {
    bool ok{false};
    std::int64_t gold{0};
};

struct UiFields {
    bool ok{false};
    /// Pathea.UISystemV2.UI.StateType (dump.cs TypeDefIndex 7275), the full enum. Six values
    /// live-confirmed 2026-11-07 (DATA-CONTRACT.md "Round 7"/"Round 8", g0 label, real New Game
    /// free roam): Gaming=0, Lock=1 (transitional, ~1 sample right after a dialogue closes),
    /// StartMenu=2 (title screen), MainMenu=6 (native pause menu, every tab), BubbleChoice=26
    /// (a dialogue choice prompt, incl. the NPC interaction wheel itself), Dialog=16 (a player
    /// self-monologue and several real NPC conversations -- Yan's full Commerce Guild intro,
    /// Mi-an's escort line and workshop tour, a wheel-initiated Chat). OrderMissionBoard=10
    /// confirmed with OrderMissionBoardUI inside the guild; Store=28 confirmed with
    /// StoreGroupUI at the saloon counter; Sleep=37 with SleepUI after the 03:00 day end,
    /// showing the new day's 07:00 clock on g0, 2026-11-09. Every other value
    /// (GiveGift=30, …) is a real, named compile-time
    /// constant from this same enum, not yet live-triggered.
    s32 state_type{-1};
};

/// Round 8: the live dialogue speaker, read directly off the on-screen UI (not off
/// NpcInteractionManager.curNpc, which Round 7 found stays null for every dialogue type reached
/// live -- see DATA-CONTRACT.md "Round 8"). Chain: UISystemMgr.Instance.curDisplayLayer.PeekState
/// (the SAME `peek` pointer ui.state_type itself reads) -> UIAssetBase.<uiRef>k__BackingField
/// (self+0x50, IUIChange) -- the instantiated UI component for whatever is currently pushed; for
/// a Dialog push this is a live Pathea.UISystemV2.UI.DialogUI instance (confirmed by class name,
/// dump.cs TypeDefIndex 7976) -> DialogUI.dialogName (self+0x78, TextMeshProUGUI) -> TMP_Text's
/// own m_text (self+0xD0, dump.cs TypeDefIndex 20372) -- the literal string on screen, e.g.
/// "Yan"/"Mi-an"/"Max" (the player's own self-talk). Live-verified against FOUR real speakers
/// this round (Yan, Mi-an, Mason, the player's own "Max"), including mid-conversation speaker
/// changes and a GamingUI (non-dialogue) cross-check that correctly returns `ok=false`.
struct DialogueFields {
    bool ok{false};            ///< a Dialog UI is currently on screen and its name text decoded
    std::string speaker_name;  ///< the literal on-screen name; "" if ok is false
    /// Text::NpcIdByName(speaker_name) -- reverse-mapped over the SAME already-loaded NPC roster
    /// NpcFields.name itself uses. -1 when the name matches no NPC (the player's own self-talk,
    /// "Max", by construction never appears in the 139-row roster) -- callers must treat this as
    /// "no NPC speaking", not an error.
    s32 npc_id{-1};
};

/// Days + hours left until a mission's submission deadline, or "no deadline" for a
/// non-positive sentinel. A passed deadline is overdue. Pure function of two raw GameDateTime
/// tick values, unit-tested directly, independent of any live host.
struct TimeLeft {
    bool has_deadline{false};
    bool overdue{false};
    s32 days{0};
    s32 hours{0};
};
TimeLeft MissionTimeLeft(u64 current_date_data, std::int64_t submit_over_time);

struct MissionFields {
    s32 mission_id{0};
    std::uint8_t mission_class{0}; ///< MissionClass (data lane fills; 0 = Other/unproven)
    std::string objective;         ///< current objective line, "" if unreadable
    std::string item_icon;         ///< sprite name of the target item, "" if none
    // Stage-9 data contract addition (2026-11-07): Mission.itemOrMonsterId/itemOrMonsterCount
    // (self+0x118/+0x11C) resolve for every running mission unconditionally, traced or not
    // (DATA-CONTRACT.md); `have` only resolves for the one traced mission (lazy InitProgress()),
    // same gap as TrackedMission.item -- `item_have_ok` is false for every other card, honestly,
    // not guessed.
    bool item_ok{false};
    std::string item_name;
    s32 item_need{0};
    s32 item_grade{-1};     ///< required quality (GradeType), -1 = none / unreadable
    bool item_have_ok{false};
    s32 item_have{0};
    std::string title;      ///< MissionNameStr_cached (dotnet string), "" if unreadable
    s32 group_type{0};      ///< MissionGroupType: -1 Normal (the story mission), 0 CommercialOrder,
                             ///< 9 MonsterMission, etc. (re/il2cpp-dump/dump.cs TypeDefIndex 6788)
    TimeLeft left;
};

struct MissionsFields {
    bool ok{false};
    std::vector<MissionFields> running; ///< m_missions_Running, in list order (not pre-sorted)
};

/// A mission's primary item/monster target progress (Mission.itemOrMonsterId/itemOrMonsterCount,
/// self+0x118/+0x11C) plus the CURRENT progress count, which only resolves once the mission's own
/// MissionTargetBase list is populated (self+0x78) -- verified live this pass: populated for the
/// one HUD-tracked mission (traceList), empty for every other running-list entry (lazy-init, only
/// traced missions get StartTrace()/InitProgress() called -- see DATA-CONTRACT.md). `have` is only
/// published when that chain resolves; `need` (and the item name) come straight off the Mission
/// object itself and are available for any mission, traced or not.
struct ItemProgress {
    bool ok{false};       ///< item_id/need resolved (Mission.itemOrMonsterId > 0)
    s32 item_id{0};
    std::string item_name; ///< static ItemPrototype name lookup (mtas_item_names.inc); "" if the
                            ///< id is not in the table (never fabricated)
    s32 need{0};           ///< Mission.itemOrMonsterCount
    s32 grade{-1};         ///< Mission.itemGradeType (+0x120, GradeType; -1 None = no quality gate)
    bool have_ok{false};   ///< the target's own `progress` list resolved (see struct comment)
    s32 have{0};
};

/// The real HUD-tracked mission (MissionManager.traceList, self+0xE0 -- replaces the old
/// groupType==-1 heuristic; see DATA-CONTRACT.md "Tracked mission" for the live verification).
struct TrackedMission {
    bool ok{false}; ///< traceList had >= 1 entry
    std::string title;
    s32 group_type{0};
    std::uint8_t mission_class{0}; ///< ComputeMissionClass(base_info_is_main, group_type)
    TimeLeft left;
    ItemProgress item;
};

// ---- Stage-9 data contract (2026-11-07). The data lane fills these; the UI lane renders a field
// only when its `ok` is true. Names/strings come from the player's own romfs at runtime (mtas_text),
// never from a table compiled into the module. Every list is capped; nothing is invented.

/// Coarse mission class for the type badge (native orange / blue / green): Main = story,
/// Side = side quest, Commission = commission/order board, Other = anything not proven.
enum class MissionClass : std::uint8_t { Other = 0, Main = 1, Side = 2, Commission = 3 };

/// Pure classifier, round-4 follow-up: `base_info_is_main` is `MissionManager.
/// allMissionBaseInfoById[missionId].IsMain` (self+0x18 on the matched `MissionBaseInfo`, dump.cs
/// TypeDefIndex 6787) -- `nullopt` when this mission has NO entry in that dictionary at all (an
/// Order/Repair/Sand/Monster mission, tracked through its own type-specific list instead, e.g.
/// `recentDeliverOrder`/`activeOrder`). This REPLACES the earlier `Mission.m_IsMain` (self+0xDC)
/// instance field, which read false on every mission across every round and turned out to be the
/// wrong signal -- `MissionBaseInfo.IsMain` is the real, STATIC per-template flag the game's own
/// Mission-tab badge uses. Live-verified this round (DATA-CONTRACT.md "Mission class"), 3 real
/// examples against the native coloured-dot badges in one boot: "Recipe for Fun"
/// (`base_info_is_main=true`) -> native ORANGE/Main; "The Request Board" and "Get Con-yak-ted"
/// (`base_info_is_main=false`) -> native BLUE/Side, both exact matches. `group_type` (raw
/// MissionGroupType, dump.cs TypeDefIndex 6788) only matters when `base_info_is_main` is
/// `nullopt`: CommercialOrder(0)/TownOrder(1) (live-confirmed groupType==0 on "Sandrock
/// Commission", native GREEN/Commission badge) map to `Commission`; anything else maps to
/// `Other` (never guessed as `Main`/`Side` without a real `MissionBaseInfo` entry).
MissionClass ComputeMissionClass(std::optional<bool> base_info_is_main, s32 group_type);

/// Round-2: a display-safe reading of FavorData.mType/mLevel (SocialNs.SocialType/SocialLevel,
/// dump.cs TypeDefIndex 12811/12812), in place of showing the raw enum ints the native UI never
/// shows directly either (the native Social card renders a heart/paw/star PIP STRIP plus a level
/// NAME, e.g. "Associate"/"Acquaintance", never "Lv. N"). See ComputeSocialHearts's own comment
/// for the exact level-range-per-type table and the live cross-check (11 NPCs, one live boot,
/// `harness/my-time-at-sandrock/runs/20261107T1319+0700-probe-default/`, native pause-menu Social
/// tab captures cross-read against `tools/probe_stage9_data.py`'s raw FavorData dump,
/// `/tmp/probe_social_round2.json`, not committed -- see DATA-CONTRACT.md "Social hearts").
struct SocialHearts {
    s32 hearts{0};      ///< filled pip count, 0..hearts_max (clamped; never negative)
    s32 hearts_max{0};  ///< pip track length for this NPC's social_type (7 Normal, 4 Lover/Mate/
                        ///< Child/Pet); 0 when social_type/level fall outside every known range
    bool stranger{false}; ///< SocialType.None(-1) / SocialLevel.None(-1): no relationship track
                          ///< established at all yet. Live-verified native treatment (Logan,
                          ///< npc_id 8011, this round's own boot): the card shows exactly ONE
                          ///< small unfilled heart glyph -- NOT a 7-pip strip at 0, no level name
                          ///< at all -- visually distinct from NormalStranger=2 (a full 7-pip
                          ///< strip, 1 filled, labelled "Acquaintance"). A UI consuming this field
                          ///< should render a stranger NPC distinctly from hearts==0, not as
                          ///< "0 of hearts_max".
};
/// Pure classifier, `dump.cs` SocialLevel/SocialType enum tables (TypeDefIndex 12811/12812):
/// Normal(0) NormalNemesis=0..FriendBest=8; Lover(1) Lover=9..LoverDeepest=12; Mate(2)
/// MateUnhappy=13..MateHappiest=16; Child(3) ChildStranger=17..ChildHappy=20; Pet(4)
/// PetStranger=21..PetLoyal=24.
///
/// Live-verified this round (11 NPCs, one boot, native pause-menu Social tab "All" grid,
/// pixel-counted pip rows cross-read against tools/probe_stage9_data.py's raw FavorData dump for
/// the same save/moment -- DATA-CONTRACT.md "Social hearts" has the full table): the Normal-type
/// pip strip is 7 pips long, not 9 (the two lowest levels, 0 NormalNemesis/1 NormalAdversary, a
/// genuinely negative standing, are not part of the positive track) -- NormalStranger=2 already
/// shows the FLOOR of that track, 1 pip filled (native label "Acquaintance"), not 0; every level
/// up to FriendBest=8 fills one more pip, reaching all 7 at the top. Formula: hearts =
/// clamp(level - floor + 1, 0, hearts_max). Confirmed exactly, every one of 11 NPCs: Heidi/
/// Arvio/Qi (mLevel=4) -> 3/7 pips; Elsie/Hugo/Burgess (mLevel=3) -> 2/7 pips (Burgess's own
/// native label "Associate"); Vivi/Unsuur/Fang/Pablo (mLevel=2) -> 1/7 pips (native label
/// "Acquaintance" on every one checked); Logan (mType=-1/mLevel=-1, the None/None sentinel) ->
/// the single-unfilled-heart stranger treatment, SocialHearts' own comment. The icon GLYPH (heart
/// vs. star) differs per NPC (Hugo/Unsuur/Nia use stars, not hearts) but the FILL COUNT formula is
/// identical either way -- the glyph choice is cosmetic, not reflected in FavorData itself (not
/// reverse engineered further this pass). The Pet family's own 4-level range (21..24) was not
/// independently re-confirmed this round (the one live Pet-type NPC available, Fennec Fox, is not
/// in this save's `mKnownList` at all -- a separate, pre-existing gap, see DATA-CONTRACT.md); kept
/// from the prior pass's reasoning (4 paw-pip cards observed in the G0 capture, consistent with a
/// 4-level family) under the same +1-floor formula as Normal, not independently proven live.
SocialHearts ComputeSocialHearts(s32 social_type, s32 level);

/// Days from `today` (day_of_world) to the next occurrence of a birthday (month 1..4, day 1..28),
/// 0..111 (this build's year is 112 days, DATA-CONTRACT.md's Fennec Fox proof); -1 if month/day is
/// out of range. Pure function of already-decoded calendar values -- shared by NpcFields'
/// days_until_birthday and ReadWeekEvents' own 7-day window.
s32 DaysUntilBirthday(u64 today, s32 birthday_month, s32 birthday_day);

/// Per-NPC social state (SocialModule.mFavorDatas). Only met NPCs are listed (spoiler rule).
struct NpcFields {
    s32 npc_id{0};
    s32 social_type{-1};
    std::string level_title;
    std::vector<s32> excellent_likes;
    std::string name;          ///< localized at runtime
    std::string portrait;      ///< sprite name for the Social-UI head (resolved by mtas_art)
    s32 favor{0};
    s32 level{0};               ///< raw FavorData.mLevel; kept for reference/debugging -- the UI
                                ///< should bind hearts/hearts_max/stranger below, not this (a raw
                                ///< -1 rendered as text, "Lv. -1" on Mason, is exactly the bug
                                ///< item 3 of this round's task fixes)
    s32 next_level_points{0};  ///< points still needed; 0 + level_max = maxed
    bool level_max{false};
    s32 hearts{0};              ///< ComputeSocialHearts(type, level).hearts -- display-safe
    s32 hearts_max{0};          ///< ComputeSocialHearts(type, level).hearts_max
    bool stranger{false};       ///< ComputeSocialHearts(type, level).stranger
    bool chatted_ok{false}, chatted_today{false};
    bool gifted_ok{false}, gifted_today{false};
    std::vector<s32> known_likes; ///< Revealed excellent/liked item IDs, in native category order.
    bool birthday_ok{false};
    s32 birthday_month{0};     ///< 1..4 (season slot), 0 = unknown
    s32 birthday_day{0};       ///< 1..28
    s32 days_until_birthday{-1}; ///< DaysUntilBirthday(today, month, day); -1 = unknown/not
                                 ///< computed this sample (birthday_ok false, or clock unresolved).
                                 ///< UI "upcoming birthday" gate: 0 <= days_until_birthday <= 6 --
                                 ///< do NOT gate on birthday_ok alone (true for ~all met NPCs with
                                 ///< a known birthday, not just the ones in the next 7 days; this
                                 ///< round's task item 4 found the UI showing a cake on every card)
};
struct SocialFields {
    bool ok{false};
    s32 met_count{0};
    std::vector<NpcFields> npcs; ///< met only
};

/// One inventory stack; source distinguishes the bag from storage containers.
struct InvItem {
    s32 item_id{0};
    std::string name;     ///< localized at runtime
    std::string icon;     ///< sprite name (ItemPrototype icon path) for mtas_art
    s32 count{0};
    s32 grade{0};         ///< quality
    std::uint32_t prototype_tags{0}, instance_tags{0}; ///< ItemPrototype/ItemInstance.tagMask
    s32 collection{0};   ///< ItemInstance.Collection: 1 collect, 2 garbage
    bool category_ok{false};
    bool in_storage{false};
};
/// StoragePartControl.CheckItem's proven ordinary filters; excludes routed containers.
bool MatchesInventoryCategory(const InvItem& item, s32 filter);
struct InventoryFields {
    bool ok{false};
    s32 bag_used{0}, bag_slots{0};
    bool storage_ok{false}; ///< the separate StorageFields chain resolved; set by Refresh()'s own
                            ///< call to MergeStorageItems, not by ReadInventory itself
    std::vector<InvItem> items; ///< bag + storage, merged per (item_id, grade, source)
};

struct CompetitorRep {
    std::string name; ///< localized workshop name, "" if unresolved
    s32 level{0};
    s32 value{0};
    bool is_player{false};
};
struct WorkshopFields {
    bool ok{false};
    s32 rank_display{0};      ///< the rank the native UI shows (verify level+1 vs level)
    s32 rep_value{0}, rep_next{0};
    std::vector<CompetitorRep> ranking; ///< player + competitors, sorted by value desc
};
struct PlayerFields {
    bool ok{false};
    s32 level{0};
    bool exp_ok{false};
    s32 exp{0}, exp_next{0};
};
struct MachineSummary {
    bool ok{false};
    s32 fuel_lack{0}, water_lack{0}, finished{0}, working{0};
};
/// A calendar event in the next 7 days (birthday of a met NPC, revealed festival, own reminder).
struct DayEvent {
    enum class Kind : std::uint8_t { Birthday, Festival, Reminder, Event } kind{Kind::Event};
    s32 day_offset{0};    ///< 0 = today
    std::string label;    ///< NPC or festival name (localized)
    std::string icon;     ///< sprite name
};
struct CalendarEvents {
    bool ok{false};
    std::vector<DayEvent> events;
};

// ---- DATA LANE round 3 (2026-11-07) additions ----

/// One required material line, shared by RecipeFields (Ingredients) and HandbookFields
/// (Handbook): resolved name/icon (mtas::GameText()) + need (static config) + "have now". Round-4
/// fix: `have` is summed across BAG (InventoryFields.items) AND every player storage box
/// (StorageFields.boxes[].items) via the shared `MergeHaveCounts` helper -- the earlier bag-only
/// sum under-counted whenever a material sat in a storage box instead of the bag (the common case
/// for a 43h save, DATA-CONTRACT.md "have counts" has the live cross-check and the native-UI/
/// game-code verification that storage genuinely counts).
struct MaterialNeed {
    s32 item_id{0};
    std::string item_name;
    std::string item_icon;
    s32 need{0};
    s32 have{0};      ///< summed bag + storage count for this item_id (any grade)
    bool can_afford{false}; ///< have >= need
};

/// One known (learned) cooking recipe, task item 6. Identified by `dish_name`/`dish_icon`
/// (resolved from the CookingModule.unlockedCookingFormulaIds entry this came from); `materials`
/// from the joined Cooking+CookingFormula static tables (mtas::GameText().RecipeByOutItem).
struct RecipeFields {
    std::string dish_name;
    std::string dish_icon;
    std::vector<MaterialNeed> materials;
    bool can_cook_now{false}; ///< every material's can_afford (false if materials is empty)
};
struct IngredientsFields {
    bool ok{false}; ///< CookingModule chain resolved (even if 0 recipes known yet)
    std::vector<RecipeFields> known; ///< one entry per learned formula with a resolved recipe
};

/// One native assembly catalogue slot; locked slots contain no item details.
/// For unlocked slots, `item_id` is the built item itself
/// (BluePrintModule.unlockedBluePrintDatas' own PrintUnlockData.itemId -- the unlock list stores
/// the ITEM id directly, not a BluePrintConfig row id, see DATA-CONTRACT.md "Handbook").
struct BlueprintFields {
    s32 item_id{0};
    std::string item_name;
    std::string item_icon;
    std::vector<MaterialNeed> materials;
    s32 category{0}; ///< CreationType: 0 Machine, 1 Other
    bool fresh{false}; ///< PrintUnlockData.isFresh (native "new blueprint" badge)
    bool unlocked{true};
};
struct HandbookFields {
    bool ok{false}; ///< BluePrintModule chain resolved
    std::vector<BlueprintFields> slots;
};

/// One player-placed storage box (task item 3). Root: `Unit<StorageUnit>.unitList` -- a SCENE
/// list (only storage units loaded in the player's current area are in this list; matches the
/// task's own "PlayerStorageObjectMgr scene objects" framing). `ok=false` whole-field if the
/// root/offsets could not be confirmed live this round (see DATA-CONTRACT.md "Storage").
struct StorageBox {
    std::string custom_name; ///< StorageUnit.customName, "" if the player hasn't renamed it
    s32 item_count{0};       ///< non-empty slot count in this box's own ItemContainer
    /// Round-4 addition: per-item breakdown (id/name/icon/count/grade), `in_storage=true` on
    /// every entry -- same `InvItem` shape and resolution path ReadInventory's own bag loop
    /// already uses, read the same way (ItemContainer.slots -> ReadOnlyItemInstanceInBag.item ->
    /// ItemInstance.prototype). Feeds MergeHaveCounts below.
    std::vector<InvItem> items;
};
struct StorageFields {
    bool ok{false};
    std::vector<StorageBox> boxes;
};

/// Pure: bag (`inventory.items`, `in_storage=false`) + every storage box's own items
/// (`storage.boxes[].items`, `in_storage=true`) summed into one item_id -> total-count map.
/// Shared by Ingredients and Handbook's own "have" computation (ReadIngredients/ReadHandbook);
/// unit-tested directly against synthetic InventoryFields/StorageFields (mtas_publish_test.cpp).
std::unordered_map<s32, s32> MergeHaveCounts(const InventoryFields& inventory,
                                             const StorageFields& storage);

/// Bug fix: wires the live Storage data (`storage`) into `inventory.items`/`storage_ok`, which
/// ReadInventory itself never touches (round-3's ReadStorage resolves a real, separate
/// StorageFields instead -- see this header's own InventoryFields comment). Appends every storage
/// box's own items (already `in_storage=true`, ReadItemContainerSlots) into `inventory.items`,
/// combining entries that share the same (item_id, grade, in_storage) into one stack with a
/// summed count rather than one duplicate row per box -- existing bag rows (`in_storage=false`)
/// are left untouched and never merge with a storage row. `inventory.storage_ok` is set to
/// `storage.ok` either way (honest "did this chain resolve", same discipline as every other `_ok`
/// field here), so the Inventory page's "Storage" filter pill lights up exactly when there is real
/// data to filter. Pure: unit-tested directly against synthetic InventoryFields/StorageFields.
void MergeStorageItems(InventoryFields& inventory, const StorageFields& storage);

/// MapModule (dump.cs TypeDefIndex 4687): shownScene +0x58, sceneConfig +0x60 (SceneConfig.nameId
/// +0x10, .scene +0x14), coordinate +0x88 (Vector4; x,y = normalised map position, v up, written
/// by SetMapOcclusion on each new 256-grid stamp cell), bitArray +0xA0 (BitArray[256]; row x,
/// bit y = a cell stamped this session; Mono BitArray m_array +0x10 -> int[8] at +0x20).
/// ArchiveMgr.self +0x70 = the loaded archive's file name (relative to "MTAS/").
struct MapMarker {
    std::string sprite;
    float x{}, y{}, z{}; // native MapIconInfo.position (world coordinates)
    float width{}, height{};
    bool in_room{false};
    std::string background;
};
struct MapFields {
    bool ok{false};       ///< MapModule.self resolved
    u64 self{0};
    u64 history{0};       ///< bitArray pointer (session identity: OnLoad re-creates it)
    s32 scene{-1};        ///< shownScene (AdditiveScene; 3 = Main)
    s32 name_id{0};       ///< sceneConfig.nameId (0 = unknown)
    bool coord_ok{false};
    float u{}, v{};
    float center_x{}, center_z{}, radius{};
    bool actor_ok{false}, facing_ok{false}, camera_ok{false};
    float actor_u{}, actor_v{}, facing_yaw{}, camera_yaw{};
    float minimap_size{}, minimap_scale{}, minimap_mask_radius{};
    std::string camera_sprite;
    bool markers_ok{false};
    std::vector<MapMarker> markers;
    std::string save_name; ///< "" when unreadable
    bool history_ok{false};
    std::vector<std::uint8_t> history_bits; ///< 256 rows x 32 bytes when history_ok
};

struct Model {
    bool game_loaded{false}; ///< Player chain resolved at all this sample (title/no-save = false)
    TimeFields time;
    BagFields bag;
    UiFields ui;
    MissionsFields missions;
    TrackedMission tracked;
    // Stage-9 contract additions (filled by the data lane; `ok` false until proven live):
    SocialFields social;
    InventoryFields inventory;
    WorkshopFields workshop;
    PlayerFields player;
    MachineSummary machines;
    CalendarEvents week;
    // DATA LANE round 3 additions:
    IngredientsFields ingredients;
    HandbookFields handbook;
    StorageFields storage;
    /// Native grade-name text ids, Grade_0..3 (GlobalModule, see SlotGlobalModule).
    bool grade_names_ok{false};
    std::array<s32, 4> grade_name_ids{};
    DialogueFields dialogue; ///< Round 8: live dialogue speaker (see DialogueFields' own comment)
};

/// Pure calendar decode from GameDateTime.dateData (the `CurrentTime` backing field), independent
/// of any live host -- unit-tested directly against the two live timestamps LIVE-READS.md
/// verified (day_of_world=100 -> Day 17/Month 4/Winter/Year 1/Wednesday; 101 -> Thursday).
struct Calendar {
    u64 day_of_world{}; ///< 0-based absolute day count; safe to add a small offset across a
                        ///< season/year boundary (used for the Today page's day strip)
    s32 year{};
    s32 day_in_season{}; ///< 1..28, this build's "day of month"
    s32 month{};
    const char* season{};
    const char* weekday{};
};
/// Pure function of an absolute day count -- the shared math CalendarFromDateData and the Today
/// page's day-strip cells both use, so a strip cell a few days from "today" decodes exactly the
/// same way "today" itself does (season/year rollover included).
Calendar CalendarFromDayOfWorld(u64 day_of_world);
Calendar CalendarFromDateData(u64 date_data);

std::string Utf16ToUtf8(std::u16string_view buf);

class Reader {
public:
    /// One refresh of the whole model. `host` is borrowed for this call only.
    Model Refresh(const EdenDsmodHostApi& host);
    /// ScenarioModule.PostingLoading plus LoadingMaskUI.playend covers the closing tween.
    /// Missing/unreadable roots are unknown, not a completed load.
    std::optional<bool> ReadLoading(const EdenDsmodHostApi& host);
    /// Map page state; `with_history` also reads the 8 KiB session stamp history.
    MapFields ReadMap(const EdenDsmodHostApi& host, bool with_history);
    /// Grade-name text ids from GlobalModule (nullopt = unreadable or not plausible text ids).
    std::optional<std::array<s32, 4>> ReadGradeNameIds(u64 main_base) const;

private:
    s32 language_{1};
    bool Read(u64 at, void* out, std::size_t n) const;
    std::optional<s32> I32(u64 at) const;
    std::optional<u64> U64(u64 at) const;
    std::optional<float> F32(u64 at) const;
    /// object/array/list/klass pointers: heap-allocated, 8-byte aligned on this target.
    static bool Ok(u64 p);
    /// C string-literal pointers (klass->name/namespace) are NOT alignment-guaranteed in this
    /// build's string blob (using the aligned check here silently dropped every class name,
    /// found during the live-memory probe that produced the offsets in this file) -- range only.
    static bool OkUnaligned(u64 p);
    std::string CStr(u64 at, std::size_t max = 96) const;
    /// Il2CppClass* -> its own class name (klass+OffName), "" if unreadable. DEAD code fix: this
    /// used to also resolve the namespace (klass+OffNamespace) on every call, discarded by every
    /// caller -- no call site in this codebase has ever compared a namespace to anything.
    std::string ClassName(u64 klass) const;
    /// .NET/IL2CPP String: length (i32) @ +0x10, UTF-16LE chars @ +0x14, decoded as UTF-8.
    std::string DotNetString(u64 p, std::size_t max_chars = 128) const;
    /// System.Collections.Generic.List<T> (reference-type T): _items @ +0x10, _size @ +0x18;
    /// Array<T>: length @ +0x18, data (8-byte elements) starting @ +0x20.
    std::vector<u64> ListItems(u64 list_ptr, std::size_t limit) const;
    /// List<IdCount> (IdCount is a VALUE-type struct {id:i32,count:i32}, 8 bytes -- same stride as
    /// a pointer slot, so this reads the same backing array as ListItems but decodes each 8-byte
    /// slot as two i32s instead of a pointer). Used for MissionTargetBase.progress (self+0x40).
    std::vector<std::pair<s32, s32>> ListIdCounts(u64 list_ptr, std::size_t limit) const;
    /// List<int> (VALUE-type int, 4-byte stride -- unlike ListItems' 8-byte pointer stride).
    /// Used for SocialModule.mKnownList (the "met NPC" id list).
    std::vector<s32> ListInt32s(u64 list_ptr, std::size_t limit) const;
    /// Same decode as ListInt32s, but distinguishes "chain resolved, list is genuinely empty"
    /// from "some internal check failed" by returning nullopt on ANY validation failure (bad
    /// items/size/capacity, or the backing Read() itself failing) instead of collapsing every
    /// failure into a silently-empty vector. Shared by ReadGiftedToday/ReadChattedToday, whose
    /// callers must tell "no one gifted/chatted today" (empty-but-present) apart from "couldn't
    /// read the list at all" (nullopt) -- see mtas_reader.cpp's own comment on both callers.
    std::optional<std::vector<s32>> ListInt32sStrict(u64 list_ptr, std::size_t limit) const;
    /// System.Collections.Generic.Dictionary<int,TRef> (standard BCL layout, confirmed from this
    /// build's own dump.cs field order, LIVE-READS.md): _entries @ +0x18, _count @ +0x20;
    /// Entry<int,TRef>: hashCode(i32)@0, next(i32)@4, key(i32)@8, value(ptr)@0x10, stride 0x18.
    /// Free-list slots (hashCode < 0, removed/never-used) are skipped, matching upstream
    /// Dictionary<K,V> semantics. Used for SocialModule.mFavorDatas.
    std::vector<std::pair<s32, u64>> DictEntries(u64 dict_ptr, std::size_t limit) const;

    /// TypeInfo slot -> klass (name checked) -> parent * hops -> static_fields. PERF: the result
    /// is cached per `slot` (root_cache_) -- a TypeInfo slot's own klass pointer, its class's
    /// parent-hop chain and its static_fields address are all TYPE-system data, not per-save
    /// instance data, so none of it can legitimately change within one boot (only the VALUE
    /// stored at static_fields+0x0, `self`, changes across a save/scene reload -- every caller
    /// already re-reads that fresh, every call, uncached). A cache hit costs two reads (the klass
    /// pointer itself + its own name pointer, re-checked to catch a stale cache) instead of the
    /// full name-string decode + hop walk + static_fields fetch.
    std::optional<u64> ResolveRoot(u64 main_base, u64 slot, const char* expect_name, int hops) const;
    struct CachedRoot {
        u64 klass{0};
        u64 name_ptr{0}; ///< klass+OffName's own raw C-string pointer, the cheap re-validate key
        u64 statics{0};
    };
    mutable std::unordered_map<u64, CachedRoot> root_cache_;

    TimeFields ReadTime(u64 main_base) const;
    BagFields ReadBag(u64 main_base) const;
    UiFields ReadUi(u64 main_base) const;
    /// Round 8: DialogueFields' own chain (UISystemMgr.Instance.curDisplayLayer.PeekState.uiRef
    /// -> DialogUI.dialogName.m_text -> Text::NpcIdByName). Independent of ReadUi (re-walks the
    /// same cheap pointer chain) so neither reader depends on the other's own guard/return shape.
    DialogueFields ReadDialogueSpeaker(u64 main_base) const;
    MissionsFields ReadMissions(u64 main_base, u64 current_date_data, bool have_clock) const;
    /// MissionManager.traceList (self+0xE0, List<Mission>) -- the real HUD tracker source.
    TrackedMission ReadTracked(u64 main_base, u64 current_date_data, bool have_clock) const;
    /// First running target's MissionTargetBase.title (+0x1C), resolved through GameText().Get()
    /// as in native GetTargetName/GetTitle. The mission's +0xF0 descriptions are synopses.
    std::optional<s32> ReadCurrentMissionTargetTitleId(u64 mission_ptr) const;
    /// Mission.itemOrMonsterId/itemOrMonsterCount (self+0x118/+0x11C, every mission) + the
    /// traced-only `have` (self+0x78 -> target+0x40 progress[0].count) -- shared by
    /// ReadTracked and ReadMissions (DATA-CONTRACT.md "Mission item lines").
    ItemProgress ReadMissionItemProgress(u64 mission_ptr) const;
    /// ItemPrototype id -> localized name, via the runtime text table (mtas::GameText(), loaded
    /// by Refresh()). "" if `item_id` is not in the table or the table has not loaded.
    static std::string ItemName(s32 item_id);
    /// Round-4 follow-up: MissionManager.allMissionBaseInfoById (self+0x20, Dictionary<int,
    /// MissionBaseInfo>) -- missionId -> MissionBaseInfo* (undereferenced; callers read `IsMain`
    /// lazily only for the handful of ids they actually have, not all ~1,069 entries). Shared by
    /// ReadMissions/ReadTracked so ComputeMissionClass's new `base_info_is_main` signal is
    /// available to both (DATA-CONTRACT.md "Mission class"). PERF: this used to rebuild the whole
    /// ~1,069-entry index from scratch on EVERY call -- twice per Refresh() (once from each of
    /// those two callers). Now cached (mission_base_info_cache_) behind a cheap fingerprint (the
    /// dictionary's own pointer + entry count); the index is only actually rebuilt when either
    /// changes, which in practice means at most once per boot for this static per-template
    /// registry, not twice per tick.
    std::unordered_map<s32, u64> ReadMissionBaseInfoIndex(u64 main_base) const;
    mutable u64 mission_base_info_dict_{0};
    mutable s32 mission_base_info_count_{-1};
    mutable std::unordered_map<s32, u64> mission_base_info_cache_;
    /// MissionBaseInfo.IsMain (self+0x18, bool) for `mission_id`, via `index`. `nullopt` if this
    /// id has no entry (an Order/Repair/Sand/Monster mission).
    std::optional<bool> MissionBaseInfoIsMain(const std::unordered_map<s32, u64>& index,
                                              s32 mission_id) const;

    // ---- Stage-9 data contract additions (2026-11-07) ----
    /// SocialModule.mFavorDatas (Dictionary<int,FavorData>, self+0xA0) filtered to
    /// SocialModule.mKnownList (self+0xC0, List<int> -- SocialModule.IsKnownNpc's own backing
    /// list; the "met" gate, LIVE-READS.md §2/DATA-CONTRACT.md).
    SocialFields ReadSocial(u64 main_base) const;
    /// Player.bag (Bag, self+0x1B0) -> storage (PlayerBagItemContainer, +0xC8) -> slots
    /// (List<ReadOnlyItemInstanceInBag>, +0x78): the 190/192-slot bag, exact cross-check in
    /// LIVE-READS.md §4. Separate player-placed storage furniture is read by ReadStorage (round
    /// 3) into its own StorageFields; `items` here stays bag-only (`in_storage=false` throughout)
    /// -- `storage_ok` and the storage rows themselves are filled in afterward, by Refresh()'s own
    /// call to MergeStorageItems, once both this and ReadStorage have resolved.
    InventoryFields ReadInventory(u64 main_base) const;
    /// Shared by ReadInventory/ReadStorage: walks one ItemContainer's own `slots`
    /// (List<ReadOnlyItemInstanceInBag>, container_ptr+0x78), resolving each non-empty slot's
    /// item id/name/icon/count/grade the same way both callers already did independently (now
    /// one implementation). `in_storage` is stamped onto every returned InvItem.
    std::vector<InvItem> ReadItemContainerSlots(u64 container_ptr, bool in_storage) const;
    /// GuildRankingManager.sortedWorkshopList (self+0x20, List<WorkshopReputation>), exact
    /// cross-check in LIVE-READS.md §5.
    WorkshopFields ReadWorkshop(u64 main_base) const;
    /// DATA LANE round 3: Player.actor (self+0x38) -> Actor.attrData (self+0x40) ->
    /// AttrData.exp/level (self+0x10/+0x14, plain public int fields) + Actor.attrCmpt (self+0x78)
    /// -> AttrCmpt.nextLevelExp (self+0x28, float; +Infinity bit pattern 0x7F800000 = maxed,
    /// AttrCmpt.get_NextLevelExp()'s own disassembled logic reproduced here -- DATA-CONTRACT.md
    /// "Player level/EXP"). Resolves the RE-REPORT.md/LIVE-READS.md §5 "BLOCKED" gap: the backing
    /// store was Actor.attrData all along, not an ActorAttr/EnumModifiableAttr indexer lookup.
    PlayerFields ReadPlayer(u64 main_base) const;
    /// MachineModule.playerMachines (self+0x38, List<MachineBase>, 4-hop Module<T> walk via
    /// ItemSystemExtendArchivableUpdatableModule<T,Prototype>), STATUS.md §13 Fix 5 /
    /// tools/probe_machines.py. Per-machine MachineSupport[] (self+0x48) read at the corrected
    /// `maximumValueFinal` offset (+0x1C), not the unmodified base `maximumValue` (+0x18) Fix 5's
    /// probe used -- see DATA-CONTRACT.md "Machines" for the dump.cs field evidence.
    MachineSummary ReadMachines(u64 main_base) const;
    /// Birthdays of met NPCs (from `social`) within the next 7 days: every npc whose own
    /// days_until_birthday (filled by Refresh(), right after ReadSocial, via the shared
    /// DaysUntilBirthday math) falls in [0,6] -- round-2: no longer re-reads TimeModule or
    /// recomputes "today" itself (the old version did, independently of NpcFields' own field,
    /// duplicated and now provably cannot disagree with it). DATA LANE round 3: `reminds` (the
    /// player's own CalendarRemindModule.reminds, pre-filtered to the next 7 days by ReadReminders
    /// below) is merged in here too. Round 4: festivals (ReadFestivals, FestivalModule) are ALSO
    /// pre-filtered and merged in via the same `reminds` vector (DATA-CONTRACT.md "Festivals" has
    /// the live verification -- this resolves the "no revealed-to-this-save gate" gap: `enable`
    /// IS the gate).
    CalendarEvents ReadWeekEvents(const SocialFields& social,
                                  const std::vector<DayEvent>& reminds) const;

    // ---- DATA LANE round 3 (2026-11-07) additions ----
    /// SendGiftModule.sentNpcId (self+0x20, List<int>): the native daily gift-limit tracker,
    /// cleared every `OnDayBegin()` (dump.cs) -- task item 1's real "gifted today" root (beyond
    /// FavorData, which has no such field, confirmed again this round). `nullopt` = the chain
    /// itself did not resolve (NOT "nobody gifted today" -- `Refresh()` must not set `gifted_ok`
    /// in that case); an empty-but-present vector IS a real "chain resolved, list just empty"
    /// result.
    std::optional<std::vector<s32>> ReadGiftedToday(u64 main_base) const;
    void ReadKnownLikes(u64 main_base, SocialFields& social) const;
    /// ConversationManager.talkedNpcId: StartChat checks this list before granting talk favor,
    /// appends the rewarded NPC, and OnPlayerWakeUpSave clears it. Missing data stays unknown.
    std::optional<std::vector<s32>> ReadChattedToday(u64 main_base) const;
    /// CookingModule.unlockedCookingFormulaIds (self+0x38, List<int>) joined against the static
    /// Cooking+CookingFormula tables (mtas::GameText().RecipeByFormulaId); "have" counts summed
    /// from `inventory` + `storage` via MergeHaveCounts (round 4: was bag-only).
    IngredientsFields ReadIngredients(u64 main_base, const InventoryFields& inventory,
                                      const StorageFields& storage) const;
    /// BluePrintModule.unlockedBluePrintDatas (self+0x28, List<PrintUnlockData>, VALUE-type
    /// struct, stride 0x18: itemId:i32@0, unlockTime:GameDateTime(u64)@8, isFresh:bool@0x10) --
    /// stores the unlocked ITEM id directly, not a BluePrintConfig row id (DATA-CONTRACT.md
    /// "Handbook" has the full reasoning for why BluePrintConfig itself is not needed). Joined
    /// against CreationConfig and CreationPartConfig (mtas::GameText().AssemblyRecipeFor); "have"
    /// counts via MergeHaveCounts (round 4: was bag-only). Locked catalogue slots carry no details.
    HandbookFields ReadHandbook(u64 main_base, const InventoryFields& inventory,
                                const StorageFields& storage) const;
    /// CalendarRemindModule.reminds (self+0x20, List<RemindSingle>, reference-type elements):
    /// the player's own "Add Reminder" entries (native Calendar tab). Each RemindSingle carries
    /// Year/Month/Day (self+0x18/0x1C/0x20) + its own RemindBasic.remindItems (self+0x10,
    /// List<RemindItem>) -- only the first item's native RemindStr label is surfaced (matches
    /// every other "first entry" convention in this reader). Filtered to the next 7 days by the
    /// caller (Refresh()), same day-window math as birthdays.
    std::vector<DayEvent> ReadReminders(u64 main_base, u64 today_day_of_world) const;
    /// Unit<StorageUnit> (0-hop closed-generic root, SlotUnitStorageUnit) -> static_fields ->
    /// `unitList` (List<StorageUnit>, scene-scoped). Per-box: StorageUnit.customName (self+0xD0)
    /// + its own Storage (ItemContainer, self+0xC0) -> slots (List<ReadOnlyItemInstanceInBag>,
    /// +0x78) non-empty count, same slot-list shape ReadInventory's own bag walk already uses.
    /// DATA-CONTRACT.md "Storage" has this round's exact live-verification result (`ok` reflects
    /// whether the static-field offset was confirmed live, not just whether the chain compiles).
    StorageFields ReadStorage(u64 main_base) const;
    /// Round-4 follow-up: FestivalModule.festivalConfigs (self+0xA8, List<FestivalConfig>) --
    /// 8 enabled, built-in yearly festivals this build (SandHold/.../SandCar, FestivalEnum).
    /// `FestivalConfig.enable` (self+0x10) is the reveal gate (task's own ask) -- skip any config
    /// read `enable=false` live (none were, this fixture; a story/DLC-locked festival could read
    /// false in principle, not observed). `start`/`end` (self+0x24/+0x30, FestivalTime's own
    /// Vector3Int fields, confirmed `(month,day,hour)` this round) are converted to the nearest
    /// occurrence via the same DaysUntilBirthday math every other calendar field in this reader
    /// already uses (festivals are yearly-recurring, same as birthdays); every day within
    /// [start_day, end_day] that falls in the next 7 days publishes one Festival DayEvent (capped
    /// at 10 days span, defensive only -- every observed festival spans 1-4 days).
    std::vector<DayEvent> ReadFestivals(u64 main_base, u64 today_day_of_world) const;

    /// PERF defensive invalidation: a `game_loaded` false->true edge (title screen -> a save
    /// loads, possibly a DIFFERENT save or build than before) clears every root/index cache
    /// above, even though none of this project's TypeInfo/class-layout data is actually expected
    /// to change within one process -- cheap insurance, not a load-bearing correctness fix (the
    /// per-call cheap re-validation in ResolveRoot already catches a stale entry on its own).
    mutable bool was_game_loaded_{false};

    const EdenDsmodHostApi* host_{};
};

} // namespace mtas
