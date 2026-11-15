// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// My Time at Sandrock (0100B63016916000) dual-screen module, update v851968, main build
// b74dfd27fc9da1b4e19d06e3d40dd370. Stage-8 slice: read-only live fields (clock/day/season/
// year/weekday from TimeModule, Gold from Player.bag, UI state from UISystemMgr) plus real art
// (parchment/day-cell sprite, gold coin icon) and the game's own font (SourceHanSans-Medium),
// all decoded from the player's own romfs at runtime -- see mtas_reader.h / mtas_art.h for the
// read/decode recipes and field contracts. Refuses any other build. HUD visibility uses the
// game's own SetActive through a leased helper; module writes stay inside its mailbox.
#include "mtas_art.h"
#include "mtas_map.h"
#include "mtas_live_fog.h"
#include "mtas_reader.h"
#include "mtas_saved_map.h"
#include "mtas_text.h"
#include "mtas_ui.h"

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace mtas {

// PERF test hook (review-L2 finding 2): counts calls into the small set of per-entry string-
// formatting primitives (Grouped/FracText/RenderIcon/RenderPortrait/GoldText/ItemLineText/
// LevelText/BadgeSrc) so a test can measure "formats per tick" before/after the render-cache fix
// below, on a synthetic model, without needing a profiler. Always compiled in (a few extra
// increments of a module-local counter are not worth an `#ifdef`'d code path to avoid), but
// nothing reads `FormatCallCount()` outside a test.
std::uint64_t g_format_calls{0};
std::uint64_t FormatCallCount() { return g_format_calls; }
void ResetFormatCallCount() { g_format_calls = 0; }

namespace {

constexpr std::uint64_t TitleId = UINT64_C(0x0100B63016916000);
// First 8 bytes of the main NSO build id b74dfd27fc9da1b4e19d06e3d40dd370.
constexpr std::uint8_t BuildId[8]{0xB7, 0x4D, 0xFD, 0x27, 0xFC, 0x9D, 0xA1, 0xB4};
constexpr int RefreshEvery = 6; // samples (60 Hz -> 10 Hz; a handful of int reads, cheap either way)
// Indices match the generator's dialogue return binds, including the three subviews.
constexpr const char* DialoguePages[]{"today", "mission", "social", "npc", "inventory",
                                      "ingredients", "workshop", "handbook", "settings", "map"};
// Map page: the session stamp history (8 KiB of guest reads) every 10 refreshes (~1 s); the
// save's fog baseline is retried every 20 refreshes (~2 s) until it decodes once per session.
constexpr std::uint64_t MapHistoryEvery = 10;
constexpr std::uint64_t MapBaselineRetryEvery = 20;
constexpr std::uint32_t MapTilePx = 128, MapDetailPx = 256;
// List caps, hoisted here (used both when building the per-refresh render cache below and when
// publishing from it) -- see Module::rendered's own comment for why these are only reformatted
// once every RefreshEvery samples instead of on every one of them.
constexpr std::size_t IngredientsMatCap = 5; // measured CookingFormula maximum for this build
constexpr std::size_t HandbookMatCap = 2;    // remaining materials have an affordability marker
constexpr std::size_t WorkshopRankCap = 32;

struct Settings {
    bool clock_top{false};
    bool tracker_top{false};
    bool dialogue_card{true};
};

Settings ReadSettings(const EdenDsmodHostApi& host) {
    // Runtime 22 reads persisted flags from the live table on the timing thread.
    const auto flag = [&](const char* key, bool initial) {
        return host.get_i64 ? host.get_i64(host.userdata, key, initial) != 0 : initial;
    };
    return {flag("@flag:mtas.clock_top", false), flag("@flag:mtas.tracker_top", false),
            flag("@flag:mtas.dialogue_card", true)};
}

/// "Day 17 (Winter Y1), Wednesday" style composed text -- built here so the manifest never has
/// to assemble several binds into one string (it can't: a label's bind_text names exactly one
/// point). Guarded identically to the raw fields: only called when both chains resolved.
std::string DaySeasonText(const TimeFields& t) {
    return UiFormat(Ui("1121"), {std::to_string(t.year),
        GameText().Get(1284 + t.month - 1), std::to_string(t.day)});
}

std::string WeekdayText(u64 day_of_world) {
    return GameText().Get(1269 + day_of_world % 7);
}

std::string DateText(const TimeFields& t) {
    return DaySeasonText(t) + " · " + WeekdayText(t.day_of_world);
}

std::string ClockText(const TimeFields& t) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", t.hour, t.minute);
    return buf;
}

/// Thousands-grouped digits; the adjacent native coin icon supplies the unit.
std::string GoldText(std::int64_t gold) {
    ++g_format_calls;
    const bool neg = gold < 0;
    std::string digits = std::to_string(neg ? -gold : gold);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            grouped += ',';
        grouped += digits[i];
    }
    return (neg ? "-" : "") + grouped;
}

/// "4 Days, 3 Hours" / "3 Hours" (days==0) / "Overdue" -- the Mission page and Today's hero card
/// share this exact text. Matches dir_b.py's own part-based "Time left:" display in spirit
/// (digits + unit words); unlike the mockup this slice does not split the leading digit into its
/// own larger token size (STATUS.md open issue -- one uniform cap height this slice).
std::string LeftText(const TimeLeft& t, bool include_label = true) {
    if (!t.has_deadline)
        return {};
    if (t.overdue)
        return Ui("overdue");
    const auto format = [&](const char* id, const std::vector<std::string>& args) {
        auto value = Ui(id);
        // Today's note already draws the native time-left label above the duration.
        if (!include_label) {
            const auto first_argument = value.find("{0}");
            if (first_argument != std::string::npos)
                value.erase(0, first_argument);
        }
        return UiFormat(value, args);
    };
    if (t.days > 0) {
        const char* id = t.days == 1 ? (t.hours == 1 ? "2048" : "2050")
                                     : (t.hours == 1 ? "2049" : "2044");
        return format(id, {std::to_string(t.days), std::to_string(t.hours)});
    }
    return format(t.hours == 1 ? "2172" : "2171", {std::to_string(t.hours)});
}

/// Native mission-type badge sprite. Stage-10: prefers the data lane's `mission_class`
/// (MissionClass: Other=0, Main=1, Side=2, Commission=3) when it names a sprite the native UI
/// actually has one for (`Main`/`Side` -- `MissionIconMain`/`MissionIconSide` are both pinned and
/// were simply unused before this pass); `Other` and `Commission` both fall through to the old
/// `groupType` heuristic rather than trusting `mission_class`'s `Other` value outright, because
/// that enum's `Other=0` is indistinguishable from "the data lane hasn't classified this mission
/// yet" (its own default-constructed value, see MissionFields's doc comment) -- and because no
/// distinct native sprite for `Commission` was found this pass (mockups/dir_b.py's own
/// `TYPE_CARD` only has three buckets: Main/Side/Other). This upgrades the live-verified Main-vs-
/// Other split (Stage-9) with a real Side badge the moment `mission_class` is filled, without
/// regressing anything already proven (STATUS.md).
std::string BadgeSrc(std::uint8_t mission_class, std::int32_t group_type) {
    ++g_format_calls;
    if (mission_class == static_cast<std::uint8_t>(MissionClass::Main))
        return "module:mtas:ui:MissionIconMain:56:56";
    if (mission_class == static_cast<std::uint8_t>(MissionClass::Side))
        return "module:mtas:ui:MissionIconSide:56:56";
    return group_type == -1 ? "module:mtas:ui:MissionIconMain:56:56"
                            : "module:mtas:ui:MissionIconOther:56:56";
}

/// Mission list order for the Mission page: deadlined missions first, soonest first; missions
/// without a deadline follow, in the order the game returned them (DIRECTIONS.md's own rule:
/// "the story mission without a deadline sorts after all deadlined ones").
std::vector<const MissionFields*> SortedMissions(const MissionsFields& missions) {
    std::vector<const MissionFields*> out;
    out.reserve(missions.running.size());
    for (const auto& m : missions.running)
        out.push_back(&m);
    std::stable_sort(out.begin(), out.end(), [](const MissionFields* a, const MissionFields* b) {
        const bool ad = a->left.has_deadline && !a->left.overdue;
        const bool bd = b->left.has_deadline && !b->left.overdue;
        if (ad != bd)
            return ad; // deadlined before not-deadlined (and before overdue)
        if (!ad)
            return false; // stable: keep original relative order among the rest
        if (a->left.days != b->left.days)
            return a->left.days < b->left.days;
        return a->left.hours < b->left.hours;
    });
    return out;
}

// ---- Stage-10 helpers (Social/Inventory/Workshop/Player/Machines pages + Today's event icons
// and machine notes row). Every one of these is a pure text/selection helper; the Stage-9 data
// contract fields they read (Model::social/inventory/workshop/player/machines/week) are the data
// lane's to fill -- each block below is gated on its own `ok` and renders nothing (not a fake
// placeholder) until that `ok` goes true, same discipline as the Stage-8/9 fields above.

/// Thousands-grouped digits, no sign handling (callers that need a sign call GoldText instead,
/// which still owns its own "-" prefix -- kept separate so this helper's contract is "just the
/// digits", reusable for always-non-negative counts like rep/exp).
std::string Grouped(std::int64_t v) {
    ++g_format_calls;
    std::string digits = std::to_string(v < 0 ? -v : v);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return (v < 0 ? "-" : "") + out;
}

std::string FracText(std::int64_t have, std::int64_t need) {
    ++g_format_calls;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s/%s", Grouped(have).c_str(), Grouped(need).c_str());
    return buf;
}

std::string LevelText(std::int32_t level) {
    ++g_format_calls;
    const auto format = Ui("2236");
    return format.find("{0}") != std::string::npos ? UiFormat(format, {std::to_string(level)})
        : format + " " + std::to_string(level);
}

/// Day-strip event icon (CalendarEvents). Birthday events get the real NPC portrait when
/// `SpriteIndex()` resolved one for that NPC (round-3: `portrait` is the bare NPC display name,
/// DATA-CONTRACT.md "Icons for UI" -- `DayEvent.icon` is already that same name, set by
/// `Reader::ReadWeekEvents` straight from the NPC's own `portrait` field); falls back to the
/// generic kind-based pin otherwise (46% of NPC portraits don't resolve yet, and Festival/
/// Reminder events have no per-entity art at all). `Festival`'s pin
/// (I_Winterfestival_icon_Fireworks_00) is this fixture's live season only -- other seasons'
/// festival art is a follow-up (same scoping as WinterRound/WinterSnowman).
std::string EventIconSrc(const DayEvent& ev) {
    if (ev.kind == DayEvent::Kind::Birthday && !ev.icon.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "module:mtas:fit:%s:66:80", ev.icon.c_str());
        return buf;
    }
    switch (ev.kind) {
    case DayEvent::Kind::Birthday:
        return "module:mtas:ui:EventBirthday:40:40";
    case DayEvent::Kind::Festival:
        return "module:mtas:ui:EventFestival:40:40";
    default: // Reminder, Event (generic) -- the same native "tip" marker
        return "module:mtas:ui:EventReminder:40:40";
    }
}

/// Pure (no host access): the resolved `module:mtas:ui:<name>:<w>:<h>` key DATA-CONTRACT.md's
/// "Icons for UI" section describes, or `ok=false` when the reader never had a name to begin
/// with (an item with neither icon path, or an NPC portrait `SpriteIndex()` coverage missed;
/// 97%/46% hit rates respectively, DATA-CONTRACT.md). Computed once per model refresh (below) and
/// replayed every tick by PublishIconRender -- `Art::LoadSprite` already forwards any such key
/// generically to `UiPins()` then `SpriteIndex()` (no module-side decode change needed), and a
/// name that doesn't resolve there fails silently (nothing drawn, never a placeholder box).
struct IconRender {
    bool ok{false};
    std::string src;
};
IconRender RenderIcon(const std::string& name, int w, int h) {
    ++g_format_calls;
    IconRender r;
    r.ok = !name.empty();
    if (r.ok) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "module:mtas:ui:%s:%d:%d", name.c_str(), w, h);
        r.src = buf;
    }
    return r;
}
/// Publishes `<key_base>_ok` (always) and `<key_base>_src` (only when `ok`) -- the engine
/// requires a republish every tick even though `r` itself is only recomputed once per refresh.
void PublishIconRender(const EdenDsmodHostApi* host, const std::string& key_base,
                       const IconRender& r) {
    host->publish_i64(host->userdata, (key_base + "_ok").c_str(), r.ok ? 1 : 0);
    if (r.ok)
        host->publish_text(host->userdata, (key_base + "_src").c_str(), r.src.c_str());
}

/// Round-4: one material line for the Ingredients/Handbook cards (DATA-CONTRACT.md's shared
/// `MaterialNeed`, now that the data lane's round-3 pass has landed it). `ok=false` (m==nullptr)
/// renders as an empty slot (no icon, no text, both affordability flags left false) -- same
/// contract PublishMaterial always had. `which` picks the ONE text variant the call site actually
/// binds (Handbook's manifest binds only `.frac_text`, Ingredients' only `.have_text` -- grepped
/// directly against `manifest.json`, neither binds `.need_text` at all) instead of formatting all
/// three unconditionally the way the original single-publish version did.
enum class MaterialTextKind { Have, Frac };
struct RenderedMaterial {
    bool ok{false};
    IconRender icon;
    std::string text; // Grouped(have) or FracText(have, need), per `which` below
    bool need_ok_green{false};
    bool need_ok_red{false};
};
RenderedMaterial RenderMaterial(const MaterialNeed* m, int icon_w, int icon_h,
                                MaterialTextKind which) {
    RenderedMaterial r;
    r.ok = m != nullptr;
    if (!r.ok)
        return r;
    r.icon = RenderIcon(m->item_icon, icon_w, icon_h);
    r.text = which == MaterialTextKind::Have ? Grouped(m->have) : FracText(m->have, m->need);
    // Two POSITIVE gates, not one `.afford` + the manifest's own "!<point>" negation: found live
    // this round that a `label` bound to a text point that was NEVER published at all (an empty
    // material slot past this recipe's own material count -- RenderMaterial's own `!ok` early
    // return, above) renders as a literal "-" dash, not nothing. `need_bind: "!recipeI.matJ.afford"`
    // therefore evaluated TRUE for an empty slot (GateOpen: a missing point reads as 0/false,
    // negated to true) and drew a stray red dash in every unused material cell
    // (ingredients-bottom.png, round-4's own first live check). Both flags are only ever
    // published when `ok`, so an empty slot now gets neither -- no widget, no stray dash.
    r.need_ok_green = m->can_afford;
    r.need_ok_red = !m->can_afford;
    return r;
}
/// `text_key` is `"have_text"` or `"frac_text"` -- whichever `RenderMaterial`'s own `which`
/// argument built `r.text` as, kept as one string so the manifest's own bind name and the
/// published key can never drift apart at a call site.
void PublishMaterial(const EdenDsmodHostApi* host, const std::string& prefix,
                     const RenderedMaterial& r, const char* text_key) {
    host->publish_i64(host->userdata, (prefix + ".ok").c_str(), r.ok ? 1 : 0);
    if (!r.ok)
        return;
    PublishIconRender(host, prefix + ".icon", r.icon);
    host->publish_text(host->userdata, (prefix + "." + text_key).c_str(), r.text.c_str());
    host->publish_i64(host->userdata, (prefix + ".need_ok_green").c_str(),
                      r.need_ok_green ? 1 : 0);
    host->publish_i64(host->userdata, (prefix + ".need_ok_red").c_str(), r.need_ok_red ? 1 : 0);
}

/// Round-4 polish (orchestrator review item 1): same contract as PublishIcon, but routes through
/// the `module:mtas:fit:<name>:<w>:<h>` key (LoadImageCallback, below) instead of
/// `module:mtas:ui:...` -- an aspect-preserving CONTAIN fit (the sprite's full native rect,
/// centred, letterboxed with transparent padding, never stretched) instead of Art::LoadSprite's
/// own nine-slice/plain-stretch path. Used only for NPC portraits: DATA-CONTRACT.md's own pin
/// table shows every resolved Social-head name is portrait-oriented (e.g. Heidi 90.9x146.9,
/// Mi-an 107.85x149.85 -- both narrower than tall) while the Social card's photo slot is
/// landscape (316x148) -- a plain stretch (the old `PublishIcon` route) squashed every face
/// ~3.5x wider than tall. The host's own `Canvas::DrawImage` always scales whatever buffer
/// `LoadImageCallback` returns to the widget's rect with no aspect handling of its own
/// (mod_ui.h's own DrawImage contract, confirmed by reading it directly this round) -- so the
/// fix has to live in the decoded buffer itself: request a buffer already sized to the exact
/// destination rect (w,h) with the real image letterboxed inside it, never ask for a mismatched
/// size and rely on the widget to fix it up. `w,h` here MUST equal the destination widget's own
/// `rect[2],rect[3]` for this to work (confirmed against npc_page()'s own detail portrait, which
/// previously requested 400x480 for a 400x340 rect -- a second, independent stretch bug fixed
/// alongside this one).
IconRender RenderPortrait(const std::string& name, int w, int h) {
    ++g_format_calls;
    IconRender r;
    r.ok = !name.empty();
    if (r.ok) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "module:mtas:fit:%s:%d:%d", name.c_str(), w, h);
        r.src = buf;
    }
    return r;
}

/// Publishes the Social-hearts display block: `<prefix>stranger`, and (only when not a stranger)
/// `<prefix>heart_src` (one native heart glyph, `SocialHeartOn` -- filled, this NPC has at least
/// one pip -- or `SocialHeartOff` when `hearts==0`) + `<prefix>hearts_text` (the plain fill-count
/// number) + `<prefix>next_level_ok`/`_text`.
///
/// Round-4 polish (orchestrator review item 1): round-3's own 7-pip strip
/// (`SocialHeartOn/Off` at 18-20px each) read below the companion's own 40px icon floor --
/// correct per DATA-CONTRACT.md's native pip-count evidence, but too small to read on a handheld
/// at that size. Switched to `mockups/dir-b-social.png`'s own form instead: one heart glyph
/// at icon-floor size (the manifest draws it >=40px) + the bare `hearts` number + "Next Level N"
/// -- still built from the exact same display-safe `hearts`/`hearts_max`/`stranger` fields
/// DATA-CONTRACT.md's "Social hearts" section describes (never the raw, sentinel-bearing
/// `level`), just a different on-screen form of the same data. A stranger (met but no
/// relationship track at all, `n.stranger`) still gets none of these: the native card shows a
/// single unfilled heart and no level name at all (live-verified via Logan, DATA-CONTRACT.md),
/// so the manifest draws that one glyph itself, unconditionally, rather than this function
/// publishing a degenerate "0 hearts" case indistinguishable from a real 0/7. `prefix` already
/// ends in whatever separator the caller wants (`"npc3."` for a repeated grid card,
/// `"npc_detail."` for the single detail view -- same fields, same meaning, one place to get the
/// math right instead of writing it out twice.
struct RenderedHearts {
    bool stranger{false};
    bool heart_ok{false};
    const char* heart_src{""}; // string-literal constants, never a dangling buffer
    std::string hearts_text;
    bool next_level_ok{false};
    std::string next_level_text;
};
RenderedHearts RenderHearts(const NpcFields& n) {
    RenderedHearts r;
    r.stranger = n.stranger;
    // `heart_ok` (round-4 bug fix, found live this round): a default-constructed NpcFields{} --
    // the "nothing selected" sentinel npc_page() publishes unconditionally against, see the
    // SampleCallback call site's own comment -- has `stranger=false` (bool default) *and*
    // `hearts_max=0`, so gating the heart glyph on `!stranger` alone (this round's first attempt)
    // drew a stray "SocialHeartOff 0" on top of the npc page's own "No townsfolk selected" empty
    // note (confirmed live, `09-07-social-after-tap-bottom.png`, before this fix). `hearts_max>0`
    // is true for every REAL non-stranger NPC (7 for Normal, 4 for Lover/Mate/Child/Pet,
    // DATA-CONTRACT.md) and false only for the sentinel (and the rare genuinely-out-of-range
    // social_type/level case, which should also show nothing) -- so AND'ing it in is the correct,
    // general fix, not a special case for the detail page alone (applies identically to the grid
    // card, where it is a no-op: repeat_bind already only expands real npc{i} slots).
    r.heart_ok = !n.stranger && n.hearts_max > 0;
    if (r.heart_ok) {
        r.heart_src = n.hearts > 0 ? "module:mtas:visible:SocialHeartOn:44:44"
                                   : "module:mtas:visible:SocialHeartOff:44:44";
        if (n.social_type == 4) r.heart_src = n.hearts > 0
            ? "module:mtas:visible:I_Social_img_Paw_00:44:44"
            : "module:mtas:visible:I_Social_img_Paw_01:44:44";
        r.hearts_text = std::to_string(n.hearts);
    }
    r.next_level_ok = !n.stranger && !n.level_max && n.hearts_max > 0;
    if (r.next_level_ok)
        r.next_level_text = Ui("2051") + " " + std::to_string(n.next_level_points);
    return r;
}
void PublishHearts(const EdenDsmodHostApi* host, const std::string& prefix,
                   const RenderedHearts& r) {
    host->publish_i64(host->userdata, (prefix + "stranger").c_str(), r.stranger ? 1 : 0);
    host->publish_i64(host->userdata, (prefix + "heart_ok").c_str(), r.heart_ok ? 1 : 0);
    if (r.heart_ok) {
        host->publish_text(host->userdata, (prefix + "heart_src").c_str(), r.heart_src);
        host->publish_text(host->userdata, (prefix + "hearts_text").c_str(), r.hearts_text.c_str());
    }
    host->publish_i64(host->userdata, (prefix + "next_level_ok").c_str(), r.next_level_ok ? 1 : 0);
    if (r.next_level_ok)
        host->publish_text(host->userdata, (prefix + "next_level_text").c_str(),
                          r.next_level_text.c_str());
}

/// "<Name>  <have>/<need>" ("<have>/<need>" alone if the name isn't in the static ItemPrototype
/// table, "<Name>  (of <need>)" if `have` never resolved -- only the one HUD-traced mission's own
/// target gets a live `have` count, see ItemProgress's own comment). Shared by Today's hero item
/// line and the Mission page's per-card item line (round-3 UI-lane icon pass: both used to inline
/// this same four-branch snprintf separately; factored out rather than duplicated a third time).
/// Player-wishes pick 1: an order's quality requirement as the game's own text ("Quality: Perfect",
/// id 1186 + the native grade name), never colour alone. Only above the base grade (Grade_0 =
/// Ordinary accepts anything), only for item orders (group 9 = monster missions, whose target id
/// is a monster), and only when the grade-name ids were read from the game.
std::string QualityText(const Model& model, s32 grade, s32 group_type, bool include_base = false) {
    if (!model.grade_names_ok || grade < (include_base ? 0 : 1) || grade > 3 || group_type == 9)
        return {};
    const auto name = GameText().Get(model.grade_name_ids[static_cast<std::size_t>(grade)]);
    const auto format = Ui("1186");
    return name.empty() || format.empty() ? std::string{} : UiFormat(format, {name});
}

std::string ItemLineText(const std::string& name, s32 need, bool have_ok, s32 have) {
    ++g_format_calls;
    char buf[96];
    if (!name.empty() && have_ok)
        std::snprintf(buf, sizeof buf, "%s  %d/%d", name.c_str(), have, need);
    else if (have_ok)
        std::snprintf(buf, sizeof buf, "%d/%d", have, need);
    else if (!name.empty())
        std::snprintf(buf, sizeof buf, "%s  (?/%d)", name.c_str(), need);
    else
        std::snprintf(buf, sizeof buf, "?/%d", need);
    return buf;
}

struct TodayHero {
    bool tracked_ok{false};
    std::string tracked_title;
    bool tracked_has_deadline{false};
    std::string tracked_left_text;
    bool tracked_item_ok{false};
    std::string tracked_item_text; ///< ItemLineText() of `tracked.item`
    std::string tracked_item_icon; ///< GameText().ItemIcon(tracked.item.item_id); "" if none
    std::string tracked_quality_text; ///< QualityText() of the tracked order, "" if none

};

/// The tracked mission readout used by the Calendar Notebook.
TodayHero ComputeToday(const TrackedMission& tracked) {
    TodayHero out;
    if (tracked.ok) {
        out.tracked_ok = true;
        out.tracked_title = tracked.title;
        out.tracked_has_deadline = tracked.left.has_deadline;
        if (tracked.left.has_deadline)
            out.tracked_left_text = LeftText(tracked.left, false);
        if (tracked.item.ok) {
            out.tracked_item_ok = true;
            out.tracked_item_text = ItemLineText(tracked.item.item_name, tracked.item.need,
                                                 tracked.item.have_ok, tracked.item.have);
            // MissionFields.item_icon is filled by the reader straight from
            // GameText().ItemIcon(item.item_id) once item.ok (mtas_reader.cpp); TrackedMission's
            // own ItemProgress carries the same item_id but no pre-resolved icon name, so resolve
            // it here the identical way rather than adding a field to a data-lane-owned struct.
            if (tracked.item.item_id > 0)
                out.tracked_item_icon = GameText().ItemIcon(tracked.item.item_id);
        }
    }
    return out;
}

/// Module::rendered's own per-entry element types -- one per list this file publishes, holding
/// exactly the formatted strings/flags that list's own publish loop needs, computed once per
/// model refresh by BuildRendered() below instead of on every one of the (up to 6x as many)
/// ticks between refreshes. Each mirrors its corresponding `0100B63016916000.cpp` publish block
/// field-for-field; see that block's own (unchanged) comments for the "why" of each value.
struct RenderedMission {
    std::string title;
    bool objective_ok{false};
    std::string objective;
    std::string badge_src;
    bool has_deadline{false};
    std::string left_text, left_value;
    bool tracked{false};
    bool item_ok{false};
    std::string item_text, item_name, item_count_text;
    std::string quality_text; ///< "" = no quality requirement shown
    IconRender icon;
};
std::vector<std::size_t> SocialOrder(const SocialFields& social, int filter) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < social.npcs.size(); ++i) {
        const auto& n = social.npcs[i];
        if (filter == 1 && (!n.gifted_ok || n.gifted_today)) continue;
        if (filter == 2 && n.social_type != 1 && n.social_type != 2) continue;
        if (filter == 3 && n.social_type != 4) continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        const auto key = [&](auto i) {
            const auto& n = social.npcs[i];
            const int birthday = n.days_until_birthday >= 0 && n.days_until_birthday <= 6
                ? n.days_until_birthday : 112;
            return std::tuple{birthday, !(n.gifted_ok && !n.gifted_today), -n.hearts};
        };
        return key(a) < key(b);
    });
    return order;
}

struct ItemUse { std::string name; s32 need; bool mission; s32 progress{-1}; };
std::vector<ItemUse> NeededFor(const Model& model, s32 item_id) {
    std::vector<ItemUse> uses;
    // Non-tracked mission targets can denote monsters; their target kind is unproven.
    const auto& tracked = model.tracked;
    if (tracked.ok && tracked.item.ok && tracked.item.item_id == item_id &&
        tracked.item.have_ok && tracked.item.need > 0)
        uses.push_back({tracked.title, tracked.item.need, true, tracked.item.have});
    for (const auto& bp : model.handbook.slots) {
        if (!bp.unlocked) continue;
        for (const auto& mat : bp.materials)
            if (mat.item_id == item_id && mat.need > 0)
                uses.push_back({bp.item_name, mat.need, false});
    }
    for (const auto& recipe : model.ingredients.known)
        for (const auto& mat : recipe.materials)
            if (mat.item_id == item_id && mat.need > 0)
                uses.push_back({recipe.dish_name, mat.need, false});
    return uses;
}

struct RenderedLike {
    std::string name;
    IconRender icon;
};
struct RenderedNpc {
    std::vector<RenderedLike> known_likes;
    std::string name;
    std::string card_name;
    IconRender social_portrait, dialogue_portrait, plan_portrait_alt, dialogue_portrait_alt;
    IconRender portrait; // grid-card size (316x148); npc_detail's own 400x340 is rendered fresh,
                         // a single cheap call for whichever one npc is selected this tick
    RenderedHearts hearts;
    bool chatted_ok{false}, chatted_today{false}, chat_pending{false};
    bool gifted_ok{false}, gifted_today{false}, gift_pending{false};
    bool birthday_ok{false};
    std::string birthday_text;
};
struct RenderedItem {
    std::string name;
    std::string count_text;
    IconRender icon; // grid size (64x64); item_detail's own 96x96 is rendered fresh, same reason
                     // as RenderedNpc::portrait above
};
struct RenderedRecipe {
    std::string name;
    IconRender icon;
    bool need_list_ok{false};
    std::string need_list_text;
    std::array<RenderedMaterial, IngredientsMatCap> mats;
    std::vector<RenderedMaterial> detail_mats;
};
struct RenderedBlueprint {
    bool unlocked{false};
    std::string name;
    IconRender icon;
    bool fresh{false};
    bool overflow_green{false}, overflow_red{false};
    bool overflow_ok{false};
    std::string overflow_text;
    std::array<RenderedMaterial, HandbookMatCap> mats;
    std::vector<RenderedMaterial> detail_mats;
};
struct RenderedRank {
    std::string pos_text;
    std::string name;
    std::string value_text;
    bool badge{false};
    std::string badge_src;
};

/// Everything SampleCallback republishes every tick, formatted exactly once per model refresh.
/// A section whose own model data never resolved (its `ok` gate false) stays default-constructed
/// (empty strings/vectors, every bool false) -- the SAME "never publish a dead end" state the old
/// per-tick code left it in, just computed once instead of up to 6 times over.
struct Rendered {
    std::string clock_text;
    std::string season_text, weekday_text, date_text, day_season_text, season_icon_src;
    struct Day {
        std::string num_text, weekday_text;
        bool has_event{false}, has_event_name{false}, birthday{false};
        std::string event_name, event_icon_src;
        int more{0}; ///< further already-revealed events that day beyond the one shown
        std::string more_text;
    };
    std::array<Day, 7> days;
    Day festival;
    bool festival_ok{false};

    std::string bag_gold_text;

    std::vector<RenderedMission> missions;
    std::string missions_count_text;
    TodayHero today;
    std::string today_tracked_badge_src;
    IconRender today_item_icon;

    bool machines_fuel_lack_bad{false}, machines_water_lack_bad{false};
    std::string machines_fuel_text, machines_water_text, machines_finished_text,
        machines_working_text;

    std::vector<RenderedNpc> npcs;
    RenderedNpc no_selection_npc;

    std::vector<RenderedItem> items;
    std::string bag_text;
    std::string storage_label, bag_label; // item_detail.source_text's two constants

    std::vector<RenderedRecipe> recipes;
    std::string ing_can_cook_text;

    std::vector<RenderedBlueprint> blueprints;
    std::string hb_category_text[3];

    std::array<RenderedRank, WorkshopRankCap> ranks;
    std::string ws_rank_text;
    bool ws_rep_ok{false};
    std::string ws_rep_text, ws_rep_needed;

    std::string player_level_text;
    std::string player_exp_text;
};

RenderedNpc RenderSocialNpc(const NpcFields& n) {
    RenderedNpc r;
    r.name = n.name.empty() ? "???" : n.name;
    r.card_name = r.name;
    r.portrait = RenderPortrait(n.portrait, 148, 142);
    r.social_portrait = RenderPortrait(n.portrait, 160, 214);
    r.plan_portrait_alt = RenderPortrait(n.portrait, 184, 176);
    r.dialogue_portrait_alt = RenderPortrait(n.portrait.empty() ? "" : n.portrait + "_Picture", 984, 670);
    r.dialogue_portrait = RenderPortrait(n.portrait.empty() ? "" : n.portrait + "_Picture", 372, 760);
    r.hearts = RenderHearts(n);
    r.chatted_ok = n.chatted_ok;
    r.chatted_today = n.chatted_ok && n.chatted_today;
    r.chat_pending = n.chatted_ok && !n.chatted_today;
    for (const auto id : n.known_likes) {
        const auto name = GameText().ItemName(id);
        r.known_likes.push_back({name, RenderIcon(GameText().ItemIcon(id), 64, 64)});
    }
    r.gifted_ok = n.gifted_ok;
    r.gifted_today = n.gifted_ok && n.gifted_today;
    r.gift_pending = n.gifted_ok && !n.gifted_today;
    const bool birthday_soon = n.days_until_birthday >= 0 && n.days_until_birthday <= 6;
    r.birthday_ok = birthday_soon;
    if (birthday_soon) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d/%d", n.birthday_month, n.birthday_day);
        r.birthday_text = buf;
    }
    return r;
}

RenderedItem RenderInventoryItem(const InvItem& it) {
    RenderedItem r;
    r.name = it.name.empty() ? "???" : it.name;
    r.count_text = Grouped(it.count);
    r.icon = RenderIcon(it.icon, 64, 64);
    return r;
}

RenderedRecipe RenderRecipe(const RecipeFields& r) {
    RenderedRecipe out;
    out.name = r.dish_name.empty() ? "???" : r.dish_name;
    out.icon = RenderIcon(r.dish_icon, 96, 96);
    for (const auto& mat : r.materials) out.detail_mats.push_back(RenderMaterial(&mat, 80, 80, MaterialTextKind::Have));
    // Round-5 polish: dir-b-ingredients.png's own "Need: Lungfish" line (the Sweet and Sour
    // Sandfish card) -- every missing material's name, comma-joined; published only when at
    // least one is actually missing (never an empty "Need:" label).
    if (!r.can_cook_now) {
        std::string need_list;
        for (const auto& mat : r.materials) {
            if (mat.can_afford || mat.item_name.empty())
                continue;
            if (!need_list.empty())
                need_list += ", ";
            need_list += mat.item_name;
        }
        if (!need_list.empty()) {
            out.need_list_ok = true;
            out.need_list_text = UiFormat(Ui("1373"), {need_list});
        }
    }
    for (std::size_t j = 0; j < IngredientsMatCap; ++j)
        out.mats[j] = RenderMaterial(j < r.materials.size() ? &r.materials[j] : nullptr, 86, 86,
                                     MaterialTextKind::Have);
    return out;
}

std::string FitRecipeNeeds(const RecipeFields& recipe, Art& art) {
    std::vector<std::string> missing;
    for (const auto& mat : recipe.materials)
        if (!mat.can_afford && !mat.item_name.empty()) missing.push_back(mat.item_name);
    const auto total = missing.size();
    while (!missing.empty()) {
        std::string names;
        for (const auto& name : missing) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        auto label = UiFormat(Ui("1373"), {names});
        if (missing.size() < total) label += " +" + std::to_string(total - missing.size());
        const auto fit = art.FitLabel(label, 424, 25);
        if (fit == label || missing.size() == 1) return fit;
        missing.pop_back();
    }
    return {};
}

RenderedBlueprint RenderBlueprint(const BlueprintFields& b) {
    RenderedBlueprint out;
    out.unlocked = b.unlocked;
    if (b.unlocked) for (const auto& mat : b.materials) out.detail_mats.push_back(RenderMaterial(&mat, 48, 48, MaterialTextKind::Frac));
    if (!b.unlocked) {
        for (std::size_t j = 0; j < HandbookMatCap; ++j)
            out.mats[j] = RenderMaterial(nullptr, 48, 48, MaterialTextKind::Frac);
        return out;
    }
    out.name = b.item_name.empty() ? "???" : b.item_name;
    out.icon = RenderIcon(b.item_icon, 96, 96);
    out.fresh = b.fresh;
    const bool overflow = b.materials.size() > HandbookMatCap;
    const bool hidden_missing = overflow &&
        std::any_of(b.materials.begin() + HandbookMatCap, b.materials.end(),
                    [](const MaterialNeed& mat) { return !mat.can_afford; });
    out.overflow_green = overflow && !hidden_missing;
    out.overflow_red = hidden_missing;
    out.overflow_ok = overflow;
    if (overflow)
        out.overflow_text = "+" + std::to_string(b.materials.size() - HandbookMatCap) + " " + Ui("more");
    for (std::size_t j = 0; j < HandbookMatCap; ++j)
        out.mats[j] = RenderMaterial(j < b.materials.size() ? &b.materials[j] : nullptr, 48, 48,
                                     MaterialTextKind::Frac);
    return out;
}

/// Builds the WHOLE per-tick-republish cache from one `Model` snapshot: every formatted string
/// (Grouped/FracText/snprintf/UiFormat/GameText().Get output) this file's SampleCallback used to
/// recompute on EVERY sample now gets computed exactly ONCE here, right after a model refresh
/// (10 Hz) -- see Module::rendered's own comment and review-L2 finding 2. Gated identically to
/// the publish conditions the old inline code used (`model.game_loaded && t.ok`, `social.ok`,
/// `inv.ok`, etc.) so a section this build's model never resolved stays a default-constructed,
/// all-empty `Rendered` section, exactly as if it had never been formatted at all -- no new
/// behaviour, only a new cadence. Pure function of `model`; never touches the host.
Rendered BuildRendered(const Model& model) {
    Rendered out;
    const auto& t = model.time;
    if (model.game_loaded && t.ok)
        out.clock_text = ClockText(t);
    if (model.game_loaded && t.calendar_ok) {
        out.season_text = GameText().Get(1284 + t.month - 1);
        out.weekday_text = WeekdayText(t.day_of_world);
        out.date_text = DateText(t);
        out.day_season_text = DaySeasonText(t);
        out.season_icon_src = "module:mtas:ui:Season" + t.season + ":44:44";
        for (int i = 0; i < 7; ++i) {
            const Calendar c = CalendarFromDayOfWorld(t.day_of_world + static_cast<u64>(i));
            auto& d = out.days[static_cast<std::size_t>(i)];
            d.num_text = std::to_string(c.day_in_season);
            d.weekday_text = WeekdayText(t.day_of_world + static_cast<u64>(i));
            const DayEvent* ev = nullptr;
            int count = 0;
            if (model.week.ok) {
                // Every entry already passed its own reveal gate (met NPC, enabled festival,
                // the player's own reminder); the first is shown, the rest are counted.
                for (const auto& e : model.week.events) {
                    if (e.day_offset == i && count++ == 0)
                        ev = &e;
                }
            }
            d.more = count > 1 ? count - 1 : 0;
            d.more_text = d.more > 0 ? "+" + std::to_string(d.more) : "";
            d.birthday = ev && ev->kind == DayEvent::Kind::Birthday;
            d.has_event = ev != nullptr;
            d.has_event_name = ev && !ev->label.empty();
            d.event_name = ev ? ev->label : "";
            if (ev)
                d.event_icon_src = EventIconSrc(*ev);
        }
    }

    if (model.game_loaded && t.calendar_ok && model.week.ok) {
        const auto found = std::find_if(model.week.events.begin(), model.week.events.end(),
            [](const auto& ev) { return ev.kind == DayEvent::Kind::Festival && ev.day_offset >= 0 && ev.day_offset <= 6; });
        if (found != model.week.events.end()) {
            out.festival_ok = true;
            auto& f = out.festival;
            const auto day = t.day_of_world + static_cast<u64>(found->day_offset);
            f.num_text = std::to_string(CalendarFromDayOfWorld(day).day_in_season);
            f.weekday_text = WeekdayText(day);
            f.event_name = found->label;
            f.has_event_name = !f.event_name.empty();
            f.has_event = true;
            f.event_icon_src = EventIconSrc(*found);
        }
    }

    if (model.bag.ok)
        out.bag_gold_text = GoldText(model.bag.gold);

    const auto& missions = model.missions;
    if (model.game_loaded && missions.ok) {
        out.missions_count_text = std::to_string(missions.running.size());
        out.missions.resize(missions.running.size());
        for (std::size_t idx = 0; idx < missions.running.size(); ++idx) {
            const auto& mf = missions.running[idx];
            auto& rm = out.missions[idx];
            rm.title = mf.title.empty() ? "???" : mf.title;
            rm.objective_ok = !mf.objective.empty();
            rm.objective = mf.objective;
            rm.badge_src = BadgeSrc(mf.mission_class, mf.group_type);
            rm.has_deadline = mf.left.has_deadline;
            if (rm.has_deadline)
                rm.left_text = LeftText(mf.left);
            if (rm.has_deadline)
                rm.left_value = LeftText(mf.left, false);
            rm.tracked = model.tracked.ok && !mf.title.empty() && mf.title == model.tracked.title;
            rm.item_ok = mf.item_ok;
            if (rm.item_ok) {
                rm.item_name = mf.item_name;
                rm.item_count_text = ItemLineText("", mf.item_need, mf.item_have_ok, mf.item_have);
                rm.item_text = ItemLineText(mf.item_name, mf.item_need, mf.item_have_ok,
                                            mf.item_have);
                rm.quality_text = QualityText(model, mf.item_grade, mf.group_type);
                rm.icon = RenderIcon(mf.item_icon, 56, 56);
            }
        }
        out.today = ComputeToday(model.tracked);
        if (out.today.tracked_item_ok)
            out.today.tracked_quality_text =
                QualityText(model, model.tracked.item.grade, model.tracked.group_type);
        if (out.today.tracked_ok)
            out.today_tracked_badge_src = BadgeSrc(0, model.tracked.group_type);
        if (out.today.tracked_item_ok)
            out.today_item_icon = RenderIcon(out.today.tracked_item_icon, 48, 48);
    }

    const auto& mach = model.machines;
    if (model.game_loaded && mach.ok) {
        out.machines_fuel_lack_bad = mach.fuel_lack > 0;
        out.machines_water_lack_bad = mach.water_lack > 0;
        out.machines_fuel_text = mach.fuel_lack > 0
                                     ? (Ui("1025") + " ×" + std::to_string(mach.fuel_lack))
                                     : Ui("ok");
        out.machines_water_text = mach.water_lack > 0
                                      ? (Ui("1025") + " ×" + std::to_string(mach.water_lack))
                                      : Ui("ok");
        out.machines_finished_text = "×" + std::to_string(mach.finished);
        out.machines_working_text = "×" + std::to_string(mach.working);
    }

    out.no_selection_npc = RenderSocialNpc(NpcFields{});
    const auto& social = model.social;
    if (social.ok) {
        out.npcs.resize(social.npcs.size());
        for (std::size_t i = 0; i < social.npcs.size(); ++i)
            out.npcs[i] = RenderSocialNpc(social.npcs[i]);
    }

    out.bag_label = Ui("1078");
    out.storage_label = Ui("2707");
    const auto& inv = model.inventory;
    if (inv.ok) {
        out.bag_text = FracText(inv.bag_used, inv.bag_slots);
        out.items.resize(inv.items.size());
        for (std::size_t i = 0; i < inv.items.size(); ++i)
            out.items[i] = RenderInventoryItem(inv.items[i]);
    }

    const auto& ing = model.ingredients;
    if (ing.ok) {
        std::int64_t can_cook_now = 0;
        for (const auto& r : ing.known)
            if (r.can_cook_now)
                ++can_cook_now;
        out.ing_can_cook_text = Ui("ready") + " " + std::to_string(can_cook_now) + " / " + std::to_string(ing.known.size());
        out.recipes.resize(ing.known.size());
        for (std::size_t i = 0; i < ing.known.size(); ++i)
            out.recipes[i] = RenderRecipe(ing.known[i]);
    }

    out.ws_rep_needed = Grouped(std::max(0, model.workshop.rep_next - model.workshop.rep_value));
    const auto& hb = model.handbook;
    if (hb.ok) {
        const char* labels[]{"52005", "1966", "1703"};
        for (int category = 0; category <= 2; ++category) {
            out.hb_category_text[category] =
                Ui(labels[category]);
        }
        out.blueprints.resize(hb.slots.size());
        for (std::size_t i = 0; i < hb.slots.size(); ++i)
            out.blueprints[i] = RenderBlueprint(hb.slots[i]);
    }

    const auto& ws = model.workshop;
    if (ws.ok) {
        out.ws_rank_text = std::to_string(ws.rank_display);
        constexpr std::int32_t RepSaneMax = 1'000'000; // generous: real values seen were ~8e3
        out.ws_rep_ok = ws.rep_value >= 0 && ws.rep_value < RepSaneMax && ws.rep_next > 0 &&
            ws.rep_next < RepSaneMax;
        if (out.ws_rep_ok)
            out.ws_rep_text = FracText(ws.rep_value, ws.rep_next);
        for (std::size_t i = 0; i < WorkshopRankCap && i < ws.ranking.size(); ++i) {
            const auto& r = ws.ranking[i];
            auto& rr = out.ranks[i];
            rr.pos_text = std::to_string(i + 1);
            rr.name = r.name.empty() ? "???" : r.name;
            rr.value_text = Grouped(r.value);
            rr.badge = i < 3;
            if (rr.badge)
                rr.badge_src = "module:mtas:ui:WsRankMargin" + std::to_string(i) + ":40:56";
        }
    }

    const auto& player = model.player;
    if (player.ok) {
        out.player_level_text = LevelText(player.level);
        if (player.exp_ok)
            out.player_exp_text = FracText(player.exp, player.exp_next);
    }
    return out;
}

bool ParseHex8(const char* s, std::uint8_t (&out)[8]) {
    if (!s || std::strlen(s) < 16)
        return false;
    for (int i = 0; i < 16; ++i) {
        const int c = std::toupper(static_cast<unsigned char>(s[i]));
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        out[i / 2] = static_cast<std::uint8_t>((i % 2 ? out[i / 2] << 4 : 0) | v);
    }
    return true;
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    std::uint8_t id[8]{};
    return ParseHex8(build_id, id) && std::memcmp(id, BuildId, 8) == 0 ? EDEN_DSMOD_TRUE
                                                                        : EDEN_DSMOD_FALSE;
}

struct Module {
    EdenDsmodHostApi host{};
    EdenDsmodHostExtensions helper{};
    Settings settings;
    Reader reader;
    std::unique_ptr<Art> art;
    Model model;
    // PERF (review-L2 finding 2): `model` only changes once every RefreshEvery samples, but the
    // engine still requires every tick's publish calls to repeat the CURRENT value. `rendered` is
    // every formatted string/flag this file's publish loops read, built once per refresh by
    // BuildRendered() instead of reformatted on each of the (up to 6x as many) ticks in between.
    // `rendered_ready` starts false (the very first sample always builds fresh, cold-start-safe)
    // and is cleared right after `model` is reassigned below -- the only two times a rebuild is
    // actually needed.
    Rendered rendered;
    bool rendered_ready{false};
    std::uint64_t samples{};
    bool loading{false};
    bool saw_loading{false};
    bool held_ok{false};
    unsigned live_refreshes{};
    // Round-4 polish: selection/filter state, driven by ActionCallback (module actions), NOT
    // `host->get_i64("@sel:<group>", ...)` -- found live this round (root-caused by reading
    // mod_module_host.cpp/mod_runtime.cpp directly, src/core/mods/) that a module's own snapshot
    // view NEVER contains "@sel:*"/"@flag:*" names: `tick_snapshot.ints` is cleared at the very
    // start of every engine tick, and `SampleState()` (which calls the module's own `sample()`
    // callback at its end) runs BEFORE `DrainTaps`/`PublishInteraction` populate this tick's
    // "@sel:" values -- so a module's `sample()` can only ever see an EMPTY "@sel:" namespace, on
    // every tick, permanently (not a timing/race issue, not fixable by waiting longer). This is
    // exactly the pattern Pokémon Sword's own module already documents and works around
    // (0100000011D90000.cpp: "the module's snapshot view has no @flag:/@sel: names") -- selection
    // has to live in the module's own state, set through a `"kind": "module"` manifest action
    // (ActionCallback), the same mechanism Sword/Monster Sanctuary already use. `select_group` is
    // still fine for same-tick PAGE rendering (a widget's own `need_bind`/`src_bind` sees this
    // tick's "@sel:" after DrainTaps, same tick) -- the gap is specifically module reads.
    std::int64_t npc_sel{-1};
    std::int64_t item_sel{-1};
    s32 social_filter{0}, mission_filter{0};
    std::int64_t mission_sel{0}, recipe_sel{0}, blueprint_sel{0};
    std::int64_t source{0}; // 0 all / 1 bag / 2 storage
    s32 category{0}; // native FilterType, ordinary item containers only
    s32 handbook_category{0}; // 0 All, 1 Machine, 2 Other
    // Social card tap used to combine `select_group` + an `on_tap` page-nav action on one widget;
    // live-traced this round (`cmd.in.out`: "DSMod select (200,250) group 'mtas_npc_sel' = 0"
    // DID fire) that this ALSO cannot work even disregarding the @sel-visibility gap above: a
    // page-changing `on_tap` triggers `ResetInteraction("page change")` on the SAME tick
    // (PublishInteraction, mod_input.cpp), which unconditionally clears the whole `selections`
    // map -- including the selection that same tick's tap just set -- before it is ever published
    // anywhere. Fix: the card's tap is now select-ONLY (a module action, no page nav of its own);
    // navigation is driven by a manifest `page_binds` entry watching this flag, which flips on
    // every selection -- `page_binds`' edge detection (mod_pages.cpp) runs independently of
    // DrainTaps' per-tap interaction reset, so it is not subject to the same clear.
    std::int32_t font_language{-1};
    std::int64_t font_epoch{};
    int label_language{-2};
    std::int64_t label_font_epoch{-1};
    std::vector<std::pair<std::string, std::string>> ui_labels;
    std::array<std::string, 7> weekday_keys, event_keys;
    bool npc_nav_parity{false};
    bool plan_nav{false};
    bool dialogue_active{false};
    std::int64_t dialogue_return_page{-1};
    bool mission_context_active{false};
    bool sleep_context_active{false};
    // Map page (tile fog, see mtas_map.h).
    EdenDsmodHostSaveApi save_api{};
    bool have_save_api{false};
    LiveFog live_fog;
    std::mutex live_fog_mutex; // load_image runs on the asset worker
    MapFields map;
    std::optional<ce_unity::Image> marker_fog;
    MapFog map_fog;
    std::uint64_t map_refreshes{};
    std::uint64_t map_baseline_attempt{~std::uint64_t{0}};
    std::vector<std::uint8_t> map_history; ///< last good session stamp history (256 x 32 bytes)
    bool map_explored_dirty{true}; ///< the live fog's explored record needs rebuilding
    std::string map_area;
    std::vector<std::string> map_marker_sources;
    mutable MapTileMask live_tiles_published; ///< live-fog tile overlays currently given a picture
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_memory ||
            std::memcmp(host->build_id, BuildId, 8) != 0)
            return nullptr;
        auto m = std::make_unique<Module>();
        m->host = *host;
        if (host->read_romfs) {
            m->art = std::make_unique<Art>(ce_unity::MakeHostRangeReader(&m->host));
        }
        return m.release();
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    auto* m = static_cast<Module*>(p);
    if (m && m->helper.store_u32) {
        m->helper.store_u32(m->helper.userdata, 12, 0);
        m->helper.store_u32(m->helper.userdata, 8, 0);
    }
    if (m) m->live_fog.Reset();
    delete m;
}

void CacheMapMarkerSources(Module& m) {
    m.map_marker_sources.clear();
    m.map_marker_sources.reserve(m.map.markers.size());
    for (const auto& marker : m.map.markers) {
        const bool area = marker.sprite.starts_with("MapIcon_MissionArea");
        const bool badge = !area && (marker.sprite.starts_with("MapIcon_Mission") || marker.in_room || !marker.background.empty());
        m.map_marker_sources.push_back(badge || area ?
            "module:mtas:badge:" + (marker.background.empty() ? std::string(marker.in_room ? "room_" : "plain_") + marker.sprite : "monster_" + marker.sprite + "~" + marker.background) + ":100:100" :
            "module:mtas:fit:" + marker.sprite + ":64:64");
    }
}

/// One refresh of the Map page state: session identity, the save's fog baseline (once per
/// session), and this session's stamp history. Fail closed: anything unread stays fogged.
void UpdateMap(Module& m, const EdenDsmodHostApi& host) {
    const bool history_due = m.map_refreshes++ % MapHistoryEvery == 0;
    auto map = m.reader.ReadMap(host, history_due);
    if (!m.model.game_loaded || !map.ok) {
        m.map = std::move(map);
        m.map.ok = false;
        return;
    }
    // A transiently unreadable archive name must not look like a different session.
    const auto& known = m.map_fog.Session();
    if (map.save_name.empty() && known.module == map.self && known.history == map.history)
        map.save_name = known.save_name;
    if (!(known == MapSession{map.self, map.history, map.save_name})) {
        m.map_history.clear();
        m.marker_fog.reset();
        { std::lock_guard lock{m.live_fog_mutex}; m.live_fog.Reset(); }
        m.map_baseline_attempt = ~std::uint64_t{0};
        m.map_explored_dirty = true;
    }
    m.map_fog.Observe({map.self, map.history, map.save_name});
    if (map.history_ok) {
        m.map_explored_dirty |= m.map_history != map.history_bits;
        m.map_history = map.history_bits;
        m.map_fog.RevealAll(TilesFromHistory(m.map_history.data(), m.map_history.size()));
    }
    const bool retry = m.map_baseline_attempt == ~std::uint64_t{0} ||
                       m.map_refreshes - m.map_baseline_attempt >= MapBaselineRetryEvery;
    if (!m.map_fog.HasBaseline() && retry && m.have_save_api && !map.save_name.empty() &&
        map.save_name.find('/') == std::string::npos) {
        m.map_baseline_attempt = m.map_refreshes;
        const auto api = m.save_api;
        const SaveMapRead read = [api](const char* path, std::uint64_t offset, void* out,
                                       std::size_t size) {
            return api.read_save_file(api.userdata, path, offset, out, size);
        };
        SavedMapCache cache;
        const auto path = "MTAS/" + map.save_name;
        cache.Poll(path, true, read);
        if (cache.Fog()) {
            // Only the tiles that look like a fog mask count: a save written without the RT
            // flushed back holds uninitialised memory (see TrustedFogBaseline).
            int rejected = 0;
            m.marker_fog = TrustedFogBaseline(*cache.Fog(), &rejected);
            const auto tiles = TilesFromFog(*m.marker_fog);
            m.map_fog.SetBaseline(tiles);
            m.map_explored_dirty = true;
            if (host.log) {
                char line[320];
                std::snprintf(line, sizeof line,
                              "MTAS map: save fog baseline %ux%u, %zu of %d tiles, %d noise tiles rejected (%s)",
                              cache.Fog()->width, cache.Fog()->height, tiles.count(), MapTiles, rejected,
                              map.save_name.c_str());
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
            }
        }
    }
    // The live fog shows only what this record knows: trusted baseline + this session's stamps.
    bool resend = false;
    {
        std::lock_guard lock{m.live_fog_mutex};
        resend = m.live_fog.Configured() && (m.map_explored_dirty || !m.live_fog.HasExplored());
    }
    if (resend) {
        auto cells = ExploredCells(m.marker_fog ? &*m.marker_fog : nullptr, m.map_history.data(),
                                   m.map_history.size());
        std::lock_guard lock{m.live_fog_mutex};
        m.live_fog.SetExplored(std::move(cells));
        m.map_explored_dirty = false;
    }
    m.map = std::move(map);
    m.map.history_bits.clear();
    m.map_area = m.map.name_id > 0 ? GameText().Get(m.map.name_id) : std::string{};
    CacheMapMarkerSources(m);
}

std::string CalendarLabelKey(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string key = "module:mtas:label:";
    for (const unsigned char c : text) { key += hex[c >> 4]; key += hex[c & 15]; }
    return key;
}

void PublishMap(const Module& m, const EdenDsmodHostApi* host) {
    struct Keys {
        std::array<std::string, MapTiles> tile, detail, tile_point, detail_point;
        Keys() {
            for (int i = 0; i < MapTiles; ++i) {
                tile[i] = MapTileKey(i, MapTilePx);
                detail[i] = MapTileKey(i, MapDetailPx);
                tile_point[i] = "map.t" + std::to_string(i);
                detail_point[i] = "map.d" + std::to_string(i);
            }
        }
    };
    static const Keys keys;
    host->publish_text(host->userdata, "map.fog_mask", m.live_fog.Ready() ? m.live_fog.Key().c_str() : "module:mtas:fog:closed");
    host->publish_i64(host->userdata, "map.fog_gated", m.live_fog.Gated());
    const bool ok = m.model.game_loaded && m.map.ok;
    host->publish_i64(host->userdata, "map.ok", ok ? 1 : 0);
    host->publish_text(host->userdata, "map.area", ok ? m.map_area.c_str() : "");
    host->publish_i64(host->userdata, "map.has_area", ok && !m.map_area.empty() ? 1 : 0);
    if (!ok)
        return;
    const bool outdoor = m.map.scene == MapSceneMain;
    host->publish_i64(host->userdata, "map.outdoor", outdoor ? 1 : 0);
    host->publish_i64(host->userdata, "map.indoors", !outdoor ? 1 : 0);
    host->publish_i64(host->userdata, "map.baseline", m.map_fog.HasBaseline() ? 1 : 0);
    const auto explored = m.map_fog.Explored();
    host->publish_i64(host->userdata, "map.explored", static_cast<std::int64_t>(explored.count()));
    if (m.live_fog.Configured()) {
        // Live fog: a tile overlay is drawn where the gated reveal shows anything, unmasked where
        // it is fully revealed, otherwise under its own mask (map.m<i>). Others get no picture.
        static const std::array<std::string, MapTiles> mask_point = [] {
            std::array<std::string, MapTiles> out;
            for (int i = 0; i < MapTiles; ++i) out[i] = "map.m" + std::to_string(i);
            return out;
        }();
        for (int i = 0; i < MapTiles; ++i) {
            const auto state = m.live_fog.Tile(i);
            const auto bit = static_cast<std::size_t>(i);
            if (state == LiveFog::TileState::Hidden) {
                if (m.live_tiles_published.test(bit))
                    host->publish_text(host->userdata, keys.tile_point[i].c_str(), "");
                m.live_tiles_published.reset(bit);
                continue;
            }
            host->publish_text(host->userdata, keys.tile_point[i].c_str(), keys.tile[i].c_str());
            host->publish_text(host->userdata, keys.detail_point[i].c_str(), keys.detail[i].c_str());
            host->publish_text(host->userdata, mask_point[i].c_str(), m.live_fog.TileMaskKey(i).c_str());
            m.live_tiles_published.set(bit);
        }
    }
    for (int i = 0; i < MapTiles; ++i) {
        if (m.live_fog.Configured() || !explored.test(static_cast<std::size_t>(i)))
            continue; // not published = not drawn = native fog shows
        host->publish_text(host->userdata, keys.tile_point[i].c_str(), keys.tile[i].c_str());
        host->publish_text(host->userdata, keys.detail_point[i].c_str(), keys.detail[i].c_str());
    }
    const bool player = outdoor && m.map.actor_ok;
    host->publish_i64(host->userdata, "map.facing_ok", player && m.map.facing_ok);
    host->publish_f64(host->userdata, "map.facing_yaw", m.map.facing_yaw);
    host->publish_i64(host->userdata, "map.camera_ok", player && m.map.camera_ok);
    host->publish_f64(host->userdata, "map.camera_yaw", m.map.camera_yaw);
    host->publish_i64(host->userdata, "map.player_ok", player ? 1 : 0);
    std::size_t plain_count = 0, badge_count = 0, area_count = 0, actor_count = 0;
    if (outdoor && m.map.radius > 0) {
        for (std::size_t index = 0; index < m.map.markers.size(); ++index) {
            const auto& marker = m.map.markers[index];
            const float u = 0.5f + (marker.x - m.map.center_x) / (2 * m.map.radius);
            const float v = 0.5f + (marker.z - m.map.center_z) / (2 * m.map.radius);
            // Live fog: a marker shows where the Map shows its ground (the gated reveal), so
            // icons follow the same rule as the terrain; otherwise the save/stamp point test.
            if (marker.sprite != "I_Map_icon_Mark_00" && m.live_fog.RevealAt(u, v) < 250 &&
                !MarkerExplored(m.marker_fog ? &*m.marker_fog : nullptr,
                                m.map_history.data(), m.map_history.size(), u, v)) continue;
            const bool area = marker.sprite.starts_with("MapIcon_MissionArea");
            const bool badge = !area && (marker.sprite.starts_with("MapIcon_Mission") || marker.in_room || !marker.background.empty());
            const bool actor = marker.sprite.ends_with("_Mini");
            auto& count = actor ? actor_count : area ? area_count : badge ? badge_count : plain_count;
            if (count >= 256) continue;
            static const auto points = [] {
                std::array<std::array<std::string, 3>, 4 * 256> result;
                constexpr const char* groups[]{"icon", "badge", "area", "actor"};
                for (int g = 0; g < 4; ++g) for (int i = 0; i < 256; ++i) {
                    const auto prefix = "map." + std::string(groups[g]) + std::to_string(i) + ".";
                    result[g * 256 + i] = {prefix + "src", prefix + "x", prefix + "y"};
                }
                return result;
            }();
            const auto& point = points[(actor ? 3 : area ? 2 : badge ? 1 : 0) * 256 + count++];
            const auto& src = m.map_marker_sources.at(index);
            host->publish_text(host->userdata, point[0].c_str(), src.c_str());
            host->publish_f64(host->userdata, point[1].c_str(), u * MapWorldUnits);
            host->publish_f64(host->userdata, point[2].c_str(), v * MapWorldUnits);
        }
    }
    host->publish_text(host->userdata, "map.clock_caption", (ClockText(m.model.time) + " " + WeekdayText(m.model.time.day_of_world)).c_str());
    host->publish_i64(host->userdata, "map.marker_count", plain_count + badge_count + area_count + actor_count);
    if (player) {
        host->publish_f64(host->userdata, "map.px", m.map.actor_u * MapWorldUnits);
        host->publish_f64(host->userdata, "map.py", m.map.actor_v * MapWorldUnits);
    }
    const char* orientation = host->get_text ? host->get_text(host->userdata, "@screen.orientation") : nullptr;
    const bool portrait = orientation && std::string_view{orientation} == "portrait";
    const double view_w = portrait ? 1080 : 1240, view_h = portrait ? 1496 : 956;
    if (player && m.map.minimap_scale > 0 && m.map.radius > 0) {
        const double x = m.map.actor_u * MapWorldUnits, y = m.map.actor_v * MapWorldUnits;
        const double ppw = m.map.minimap_scale * (2 * m.map.radius) / MapWorldUnits;
        const double hw = view_w / (2 * ppw), hh = view_h / (2 * ppw);
        host->publish_f64(host->userdata, "map.vx0", x - hw);
        host->publish_f64(host->userdata, "map.vx1", x + hw);
        host->publish_f64(host->userdata, "map.vy0", y - hh);
        host->publish_f64(host->userdata, "map.vy1", y + hh);
        host->publish_f64(host->userdata, "map.default_ppw", ppw);
    }
    bool cone = false;
    if (player && m.map.camera_ok && host->get_f64) {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        const double cx = host->get_f64(host->userdata, "@map_view_cx:sandrock", missing);
        const double cy = host->get_f64(host->userdata, "@map_view_cy:sandrock", missing);
        const double ppw = host->get_f64(host->userdata, "@map_view_ppw:sandrock", missing);
        if (std::isfinite(cx) && std::isfinite(cy) && ppw > 0) {
            const double x = view_w / 2 + (m.map.actor_u * MapWorldUnits - cx) * ppw;
            const double y = view_h / 2 - (m.map.actor_v * MapWorldUnits - cy) * ppw;
            host->publish_f64(host->userdata, "map.screen_x", x);
            host->publish_f64(host->userdata, "map.screen_y", y);
            cone = x >= 74 && x <= view_w - 74 && y >= 112 && y <= view_h - 112;
        }
    }
    host->publish_i64(host->userdata, "map.cone_visible", cone);

}

// A reload may attach after the loading transition. Require consecutive known-closed reads
// and live gameplay/date signals so title singletons cannot qualify with boot defaults.
void AcceptFreshModel(Module& m, Model& fresh, std::optional<bool> loading) {
    const bool live = loading == false && fresh.game_loaded && fresh.ui.ok &&
                      fresh.ui.state_type == 0 && fresh.time.ok && fresh.time.calendar_ok &&
                      fresh.time.day > 0;
    m.live_refreshes = live ? std::min(m.live_refreshes + 1, 3u) : 0;
    // StartMenu is the title; MainMenu (6) is the native pause menu above a loaded save.
    const bool title = fresh.ui.ok && fresh.ui.state_type == 2;
    if (!fresh.game_loaded || title) {
        m.held_ok = false;
        m.saw_loading = false;
        m.live_refreshes = 0;
        const auto ui = fresh.ui;
        fresh = {};
        fresh.ui = ui;
        m.model = {};
    } else if (m.saw_loading || m.held_ok || m.live_refreshes == 3) {
        m.held_ok = true;
    } else {
        fresh = {};
    }
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (!p || !host)
            return;
        auto& m = *static_cast<Module*>(p);
        const auto& settings = m.settings;
        std::uint32_t magic{}, hidden{};
        const bool helper_live = m.helper.load_u32 &&
            m.helper.load_u32(m.helper.userdata, 0, &magic) && magic == 0x4D544831;
        if (helper_live)
            m.helper.load_u32(m.helper.userdata, 16, &hidden);
        host->publish_i64(host->userdata, "mtas.hud_supported", helper_live);
        host->publish_i64(host->userdata, "mtas.hud_hidden", hidden);
        if (m.samples++ % RefreshEvery == 0) {
            const auto loading = m.reader.ReadLoading(*host);
            if (loading) m.loading = *loading;
            if (loading != false) m.live_refreshes = 0;
            m.saw_loading = m.saw_loading || m.loading;
            // Republish the held model each tick; refresh only after the mask closes.
            if (!m.loading) {
                auto fresh = m.reader.Refresh(*host);
                AcceptFreshModel(m, fresh, loading);
                m.model = std::move(fresh);
                m.rendered_ready = false; // the new model has nothing formatted for it yet
                UpdateMap(m, *host);
            }
        }
        {
            std::lock_guard lock{m.live_fog_mutex};
            const auto previous_fog_key = m.live_fog.Key();
            const bool fog_was_ready = m.live_fog.Ready();
            m.live_fog.Refresh(m.model.game_loaded && !m.loading && m.map.ok);
            if (m.live_fog.Ready() && (!fog_was_ready || m.live_fog.Key() != previous_fog_key) && host->log) {
                int drawn = 0, masked = 0;
                for (int i = 0; i < MapTiles; ++i) {
                    drawn += m.live_fog.Tile(i) != LiveFog::TileState::Hidden;
                    masked += m.live_fog.Tile(i) == LiveFog::TileState::Masked;
                }
                char line[256];
                std::snprintf(line, sizeof line,
                              "MTAS live fog: generation=%llu decode=%.1fus refresh=%.1fus "
                              "explored=%d gated=%d tiles=%d masked=%d",
                              static_cast<unsigned long long>(m.live_fog.Generation()),
                              m.live_fog.CostUs(), m.live_fog.RefreshUs(), m.live_fog.HasExplored(),
                              m.live_fog.Gated(), drawn, masked);
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO, line);
            }
        }
        if (!m.rendered_ready) {
            m.rendered = BuildRendered(m.model);
            for (std::size_t i = 0; i < 7; ++i) {
                m.weekday_keys[i] = CalendarLabelKey(m.rendered.days[i].weekday_text);
                m.event_keys[i] = CalendarLabelKey(m.rendered.days[i].event_name);
            }
            if (m.art) {
                for (auto& day : m.rendered.days) {
                    // Calendar text fits in the active widget, preserving its localized source.
                    (void)day;
                }
                for (auto& npc : m.rendered.npcs) npc.card_name = m.art->FitLabel(npc.name, 170, 25);
                for (std::size_t i = 0; i < m.rendered.recipes.size(); ++i)
                    if (m.rendered.recipes[i].need_list_ok)
                        m.rendered.recipes[i].need_list_text = FitRecipeNeeds(m.model.ingredients.known[i], *m.art);
            }
            m.rendered_ready = true;
        }
        if (m.art && GameText().Loaded() && m.font_language != GameText().CurrentLanguage()) {
            auto characters = GameText().FontText();
            for (const auto& row : UiOwnLabels)
                characters += Ui(row.key);
            m.art->SetFontText(characters);
            m.font_language = GameText().CurrentLanguage();
            ++m.font_epoch;
        }
        host->publish_i64(host->userdata, "__font_epoch", m.font_epoch);
        host->publish_i64(host->userdata, "mtas.loading", m.loading);
        PublishMap(m, host);
        const int label_language = GameText().Loaded() ? GameText().CurrentLanguage() : -1;
        if (m.label_language != label_language || m.label_font_epoch != m.font_epoch) {
            m.ui_labels.clear();
            for (int id : UiGameIds) {
                const auto key = std::to_string(id);
                m.ui_labels.emplace_back("ui." + key, Ui(key));
            }
            for (const auto& row : UiOwnLabels)
                m.ui_labels.emplace_back(std::string("ui.") + row.key, Ui(row.key));
            m.label_language = label_language;
            m.label_font_epoch = m.font_epoch;
        }
        for (const auto& [key, value] : m.ui_labels)
            host->publish_text(host->userdata, key.c_str(), value.c_str());
        const auto& t = m.model.time;
        const auto& b = m.model.bag;
        const auto& ui = m.model.ui;
        host->publish_i64(host->userdata, "today.weather_ok", m.model.game_loaded && t.weather_ok);
        if (m.model.game_loaded && t.weather_ok) {
            constexpr const char* icons[]{"Sunny_00", "Rainy_00", "Sandstorm_00", "Overcast_00", "Sunny_01", "Snow_00"};
            if (t.weather >= 0 && t.weather < 6)
                host->publish_text(host->userdata, "today.weather_src",
                    (std::string("module:mtas:ui:I_Main_img_Weather") + icons[t.weather] + ":40:40").c_str());
        }
        const int workshop_rank = m.model.workshop.rank_display;
        host->publish_i64(host->userdata, "ws.rank_ok", m.model.game_loaded && m.model.workshop.ok && workshop_rank >= 1 && workshop_rank <= 5);
        host->publish_text(host->userdata, "ws.rank_icon_src",
            ("module:mtas:ui:I_Playerinfo_icon_Star_0" + std::to_string(std::clamp(workshop_rank, 1, 5)) + ":80:80").c_str());
        for (int i = 0; i < 5; ++i)
            host->publish_text(host->userdata, ("ws.star" + std::to_string(i) + "_src").c_str(),
                (std::string("module:mtas:rank_star:") + (i < std::clamp(m.model.workshop.rank_display, 0, 5) ? "filled" : "empty")).c_str());
        host->publish_i64(host->userdata, "mtas.game_loaded", m.model.game_loaded ? 1 : 0);
        host->publish_i64(host->userdata, "mtas.held_ok", m.held_ok ? 1 : 0);
        // Gate the whole Today readout on game_loaded (Player.bag resolving), not on TimeModule's
        // own chain alone: TimeModule.Self stays a live, readable singleton even at the title
        // menu (CurDay/Hour/Minute hold boot-default zeros there, not null) -- found live this
        // session (title-screen capture showed a coherent but meaningless "Monday, Day 0 (Spring
        // Y1)" while Bag correctly failed to resolve at all). Player.bag not resolving is this
        // game's actual "no save loaded" signal; reuse it for Time too so the whole card goes
        // blank together at title instead of showing boot defaults that look like real data.
        const auto& rendered = m.rendered;
        const char* orientation = host->get_text ? host->get_text(host->userdata, "@screen.orientation") : nullptr;
        const bool portrait_canvas = orientation && std::string_view{orientation} == "portrait";
        host->publish_i64(host->userdata, "today.week7", m.model.game_loaded && !rendered.festival_ok);
        host->publish_i64(host->userdata, "today.week8", m.model.game_loaded && rendered.festival_ok);
        if (m.model.game_loaded && t.ok) {
            host->publish_i64(host->userdata, "time.day", t.day);
            host->publish_i64(host->userdata, "time.hour", t.hour);
            host->publish_i64(host->userdata, "time.minute", t.minute);
            host->publish_text(host->userdata, "time.clock_text", rendered.clock_text.c_str());
        }
        if (m.model.game_loaded && t.calendar_ok) {
            host->publish_text(host->userdata, "time.weekday", rendered.weekday_text.c_str());
            host->publish_text(host->userdata, "time.date_text", rendered.date_text.c_str());
            // Today-polish (Stage-10): the date note's own two-line split (bold date / plain
            // weekday below), matching the mockup's "Day 17 Winter" + "Wednesday" rows -- see
            // DaySeasonText()'s own comment. `time.date_text` (combined) is left published too;
            // nothing currently binds to it, kept for the fidelity composite's own reference.
            host->publish_text(host->userdata, "today.day_season_text",
                              rendered.day_season_text.c_str());
            // Season icon: a full module: image key chosen by the live season string, so the
            // manifest's image widget (src_bind) follows whichever season is actually live
            // instead of only ever showing Winter (this fixture's season). No live weather chain
            // was found this pass (STATUS.md open issue) -- the weather well is omitted rather
            // than showing a fixed/guessed icon.
            host->publish_text(host->userdata, "today.season_icon_src",
                              rendered.season_icon_src.c_str());
            // 7-day strip (Stage-9 fidelity round, up from 6 -- DIRECTIONS.md's own "6-7 cells"):
            // CalendarFromDayOfWorld on today's own day_of_world + 0..6, the exact same decode
            // "today" itself uses, so a cell crossing a season/year boundary still reads correctly
            // (season rollover included) -- not just CurDay+offset, which would break at the
            // 28-day season edge. Birthdays/festivals are not wired this slice (no live "met NPC"
            // + NpcProtoData birthday decode yet, see STATUS.md open issue); each cell is day
            // number + weekday only. Formatted once per refresh (BuildRendered); republished
            // as-is here every tick.
            for (int i = 0; i < 7; ++i) {
                const auto& d = rendered.days[static_cast<std::size_t>(i)];
                static const std::array<std::string, 7> prefixes = [] {
                    std::array<std::string, 7> out;
                    for (int day = 0; day < 7; ++day) out[day] = "day" + std::to_string(day) + ".";
                    return out;
                }();
                const auto& p = prefixes[i];
                host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                host->publish_text(host->userdata, (p + "num").c_str(), d.num_text.c_str());
                // Keep the complete localized weekday; the manifest fits it to the cell.
                host->publish_text(host->userdata, (p + "weekday").c_str(), d.weekday_text.c_str());
                host->publish_text(host->userdata, (p + "weekday_src").c_str(), m.weekday_keys[i].c_str());
                host->publish_text(host->userdata, (p + "event_name_src").c_str(), m.event_keys[i].c_str());
                // CalendarEvents day_offset 0 is today; unavailable weeks hide both icon and name.
                host->publish_i64(host->userdata, (p + "birthday").c_str(), d.birthday);
                host->publish_i64(host->userdata, (p + "has_event").c_str(), d.has_event ? 1 : 0);
                host->publish_i64(host->userdata, (p + "has_event_name").c_str(),
                                  d.has_event_name ? 1 : 0);
                host->publish_text(host->userdata, (p + "event_name").c_str(),
                                   d.event_name.c_str());
                host->publish_i64(host->userdata, (p + "has_more").c_str(), d.more > 0 ? 1 : 0);
                host->publish_text(host->userdata, (p + "more_text").c_str(), d.more_text.c_str());
                if (d.has_event) {
                    host->publish_text(host->userdata, (p + "event_icon_src").c_str(),
                                       d.event_icon_src.c_str());
                }
            }
        }
        if (rendered.festival_ok) {
            const auto& f = rendered.festival;
            host->publish_text(host->userdata, "festival.num", f.num_text.c_str());
            host->publish_text(host->userdata, "festival.weekday", f.weekday_text.c_str());
            host->publish_text(host->userdata, "festival.event_name", f.event_name.c_str());
            host->publish_text(host->userdata, "festival.event_icon_src", f.event_icon_src.c_str());
        }
        host->publish_i64(host->userdata, "festival.has_event", rendered.festival_ok);
        for (const int cells : {7, 8}) {
            const bool visible = m.model.game_loaded && (rendered.festival_ok == (cells == 8));
            const std::string base = cells == 7 ? "calendar7." : "calendar8.";
            host->publish_i64(host->userdata, (base + "all").c_str(), visible);
            for (int i = 0; i < cells; ++i) {
                const auto& d = i < 7 ? rendered.days[i] : rendered.festival;
                static const auto prefixes = [] {
                    std::array<std::array<std::string, 8>, 2> out;
                    for (int c = 0; c < 2; ++c) for (int day = 0; day < 8; ++day)
                        out[c][day] = "calendar" + std::to_string(c + 7) + "." +
                            (day < 7 ? "day" + std::to_string(day) + "." : "festival.");
                    return out;
                }();
                const auto& prefix = prefixes[cells - 7][i];
                host->publish_i64(host->userdata, (prefix + "birthday").c_str(), visible && d.birthday);
                host->publish_i64(host->userdata, (prefix + "has_event").c_str(), visible && d.has_event);
                host->publish_i64(host->userdata, (prefix + "has_event_name").c_str(), visible && d.has_event_name);
            }
        }
        if (m.model.game_loaded && b.ok) {
            host->publish_i64(host->userdata, "bag.gold", b.gold);
            host->publish_text(host->userdata, "bag.gold_text", rendered.bag_gold_text.c_str());
        }
        if (ui.ok) {
            host->publish_i64(host->userdata, "ui.state_type", ui.state_type);
            host->publish_i64(host->userdata, "ui.gaming", ui.state_type == 0 ? 1 : 0);
        }
        // ---- Dialogue auto-context (Round 8): a live Dialog UI whose own on-screen name
        // resolves to a met NPC (mtas_reader.h's DialogueFields, live-verified this round
        // against four real speakers -- DATA-CONTRACT.md "Round 8") takes over the NPC-detail
        // page's own selection below, via `dialogue_npc_sel` (an index into social.npcs, same
        // unit `m.npc_sel` already uses -- NOT the raw npc id). Published unconditionally
        // (not gated on `social.ok`) so a page_bind watching it always sees a real 0/1, never a
        // "missing" point mid-dialogue -- mod_pages.cpp's own page_binds only treats a value
        // actually read this tick as an edge; a point that silently stops publishing re-arms
        // from whatever it reads next WITHOUT firing, which would leave the auto-shown card
        // stuck on screen instead of returning to the prior page when the dialogue ends. -1 covers both
        // "no dialogue" and "the speaker isn't a met NPC" (the player's own self-talk, "Max",
        // never matches any roster entry).
        std::int64_t dialogue_npc_sel = -1;
        if (m.model.game_loaded && settings.dialogue_card && m.model.dialogue.ok && m.model.dialogue.npc_id >= 0) {
            const auto& dsocial = m.model.social;
            for (std::size_t i = 0; i < dsocial.npcs.size(); ++i) {
                if (dsocial.npcs[i].npc_id == m.model.dialogue.npc_id) {
                    dialogue_npc_sel = static_cast<std::int64_t>(i);
                    break;
                }
            }
        }
        const bool dialogue_active = dialogue_npc_sel >= 0;
        // StateType 10 / OrderMissionBoardUI, 28 / StoreGroupUI and 37 / SleepUI, live on g0.
        // Share the return page with dialogue so direct transitions preserve the original page.
        // Keep the context across unreadable samples until a known state closes it.
        const bool mission_context_active = m.model.game_loaded &&
            (ui.ok ? (ui.state_type == 10 || ui.state_type == 28) : m.mission_context_active);
        const bool sleep_context_active = m.model.game_loaded &&
            (ui.ok ? ui.state_type == 37 : m.sleep_context_active);
        const bool context_active = dialogue_active || mission_context_active || sleep_context_active;
        if (context_active && !(m.dialogue_active || m.mission_context_active || m.sleep_context_active)) {
            m.dialogue_return_page = -1;
            // The host answers this session query directly, independently of the sample snapshot.
            const char* page = host->get_text ? host->get_text(host->userdata, "__page") : nullptr;
            for (std::size_t i = 0; page && i < std::size(DialoguePages); ++i) {
                if (std::strcmp(page, DialoguePages[i]) == 0) {
                    m.dialogue_return_page = static_cast<std::int64_t>(i);
                    break;
                }
            }
        }
        m.dialogue_active = dialogue_active;
        m.mission_context_active = mission_context_active;
        m.sleep_context_active = sleep_context_active;
        // Older hosts without the query skip auto-navigation rather than lose the player's page.
        host->publish_i64(host->userdata, "dlg.active",
                          dialogue_active && m.dialogue_return_page >= 0 ? 1 : 0);
        host->publish_i64(host->userdata, "context.mission",
                          mission_context_active && m.dialogue_return_page >= 0 ? 1 : 0);
        host->publish_i64(host->userdata, "context.today",
                          sleep_context_active && m.dialogue_return_page >= 0 ? 1 : 0);
        // Hold -1 throughout any context so repeated returns to the same page have an edge.
        host->publish_i64(host->userdata, "dlg.return_page",
                          context_active ? -1 : m.dialogue_return_page);
        const auto& missions = m.model.missions;
        if (m.model.game_loaded && missions.ok) {
            host->publish_i64(host->userdata, "missions.count",
                              static_cast<std::int64_t>(missions.running.size()));
            // label/bind_text only resolves TEXT points (value/bind is for numbers) -- publish a
            // text twin of every integer a label on these two pages displays.
            host->publish_text(host->userdata, "missions.count_text",
                              rendered.missions_count_text.c_str());
            host->publish_text(host->userdata, "missions.header_text",
                              (Ui("1039") + "  " + rendered.missions_count_text).c_str());
            // Sorting is a cheap pointer comparison (no formatting), so it still runs every tick;
            // only the PER-MISSION TEXT below (formatted once per refresh by BuildRendered) is
            // looked up by original index rather than reformatted here.
            auto sorted = SortedMissions(missions);
            if (m.mission_filter) std::erase_if(sorted, [&](const auto* mission) {
                return mission->mission_class != m.mission_filter;
            });
            host->publish_i64(host->userdata, "missions.count", sorted.size());
            host->publish_i64(host->userdata, "mission_detail.ok", !sorted.empty());
            for (int f = 0; f < 4; ++f) {
                const auto p = "missions.filter" + std::to_string(f);
                host->publish_i64(host->userdata, (p + ".selected").c_str(), f == m.mission_filter);
                host->publish_i64(host->userdata, (p + ".idle").c_str(), f != m.mission_filter);
            }
            for (std::size_t i = 0; i < sorted.size(); ++i) {
                auto prefixes = std::vector{std::string("mission") + std::to_string(i) + "."};
                const auto selected = std::min(static_cast<std::size_t>(m.mission_sel), sorted.size() - 1);
                if (i == selected) prefixes.push_back("mission_detail.");
                host->publish_i64(host->userdata, (prefixes.front() + "selected").c_str(), i == selected);
                for (const auto& p : prefixes) {
                    const auto idx = static_cast<std::size_t>(sorted[i] - missions.running.data());
                    const auto& rm = rendered.missions[idx];
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "title").c_str(), rm.title.c_str());
                    host->publish_i64(host->userdata, (p + "objective_ok").c_str(),
                                      rm.objective_ok ? 1 : 0);
                    if (rm.objective_ok)
                        host->publish_text(host->userdata, (p + "objective").c_str(),
                                          rm.objective.c_str());
                    host->publish_text(host->userdata, (p + "badge_src").c_str(), rm.badge_src.c_str());
                    host->publish_i64(host->userdata, (p + "has_deadline").c_str(),
                                      rm.has_deadline ? 1 : 0);
                    if (rm.has_deadline)
                        host->publish_text(host->userdata, (p + "left_text").c_str(),
                                          rm.left_text.c_str());
                    if (rm.has_deadline)
                        host->publish_text(host->userdata, (p + "left_value").c_str(),
                                          rm.left_value.c_str());
                    // Mission-page "tracked mission marked" (Stage-10): matched by title against
                    // the real HUD tracker (MissionManager.traceList, see mtas_reader.h) -- the
                    // only identity MissionFields/TrackedMission share (TrackedMission carries no
                    // pointer; see DATA-CONTRACT.md). Titles are the game's own mission names,
                    // effectively unique within one running list.
                    host->publish_i64(host->userdata, (p + "tracked").c_str(), rm.tracked ? 1 : 0);
                    const bool owned = rm.tracked && m.model.inventory.ok && m.model.tracked.item.ok;
                    host->publish_i64(host->userdata, (p + "owned_ok").c_str(), owned);
                    if (owned) {
                        std::int64_t bag = 0, storage = 0;
                        for (const auto& stack : m.model.inventory.items)
                            if (stack.item_id == m.model.tracked.item.item_id)
                                (stack.in_storage ? storage : bag) += stack.count;
                        host->publish_text(host->userdata, (p + "bag_count").c_str(), std::to_string(bag).c_str());
                        host->publish_text(host->userdata, (p + "storage_count").c_str(), std::to_string(storage).c_str());
                        host->publish_i64(host->userdata, (p + "storage_ok").c_str(), m.model.inventory.storage_ok);
                        host->publish_text(host->userdata, (p + "owned_note").c_str(), UiFormat(Ui("1186"), {Ui("52005")}).c_str());
                    }

                    // Item icon/n-m line (round-3): MissionFields.item_ok/item_name/item_need
                    // resolve for every running mission unconditionally (mtas_reader.h's own
                    // comment); only `item_have`/`item_have_ok` are traced-mission-only, which
                    // ItemLineText already degrades gracefully without (the "(of N)" branch).
                    host->publish_i64(host->userdata, (p + "item_ok").c_str(), rm.item_ok ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "objective_item").c_str(), rm.objective_ok && rm.item_ok);
                    host->publish_i64(host->userdata, (p + "objective_plain").c_str(), rm.objective_ok && !rm.item_ok);
                    const bool quality = rm.item_ok && !rm.quality_text.empty();
                    host->publish_i64(host->userdata, (p + "has_quality").c_str(), quality ? 1 : 0);
                    // The objective drops one row (and to two lines) while a quality row shows.
                    host->publish_i64(host->userdata, (p + "objective_q").c_str(),
                                      rm.objective_ok && quality ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "objective_nq").c_str(),
                                      rm.objective_ok && !quality ? 1 : 0);
                    host->publish_text(host->userdata, (p + "quality_text").c_str(),
                                       rm.quality_text.c_str());
                    if (rm.item_ok) {
                        host->publish_text(host->userdata, (p + "item_text").c_str(),
                                          rm.item_text.c_str());
                        host->publish_text(host->userdata, (p + "item_name").c_str(), rm.item_name.c_str());
                        host->publish_text(host->userdata, (p + "item_count_text").c_str(), rm.item_count_text.c_str());
                        PublishIconRender(host, p + "icon", rm.icon);
                    }
                }
            }
            const auto& today = rendered.today;
            if (today.tracked_ok) {
                host->publish_i64(host->userdata, "today.tracked_ok", 1);
                host->publish_text(host->userdata, "today.tracked_title",
                                  today.tracked_title.c_str());
                // TrackedMission carries no mission_class field (only MissionFields does, see
                // mtas_reader.h) -- 0/Other falls through to the group_type heuristic, same as
                // Stage-9 (unchanged live behaviour for Today).
                host->publish_text(host->userdata, "today.tracked_badge_src",
                                  rendered.today_tracked_badge_src.c_str());
                host->publish_i64(host->userdata, "today.tracked_has_deadline",
                                  today.tracked_has_deadline ? 1 : 0);
                if (today.tracked_has_deadline)
                    host->publish_text(host->userdata, "today.tracked_left_text",
                                      today.tracked_left_text.c_str());
                host->publish_i64(host->userdata, "today.tracked_has_quality",
                                  today.tracked_item_ok && !today.tracked_quality_text.empty());
                host->publish_text(host->userdata, "today.tracked_quality_text",
                                   today.tracked_quality_text.c_str());
                if (today.tracked_item_ok) {
                    host->publish_i64(host->userdata, "today.tracked_item_ok", 1);
                    host->publish_text(host->userdata, "today.tracked_item_text",
                                      today.tracked_item_text.c_str());
                    PublishIconRender(host, "today.tracked_item_icon", rendered.today_item_icon);
                }
            }
        }

        // ---- Machine notes (Today "half-empty page" fix, real this time, + Workshop's own
        // machine row -- both pages bind the same machines.* fields). `today.show_fallback_note`
        // is always published (1 = show the old "In Progress <N>" note) so the two notes stay
        // mutually exclusive with a single need_bind each; `mtas.machines_ok` likewise always
        // published (never only inside the `ok` branch) so the negated gate has something to read
        // before the data lane's MachineModule chain ever resolves. Both are ANDed with
        // `game_loaded`, not `mach.ok` alone: found live this pass (pregame-state screenshot) that
        // `MachineModule`'s own chain, like `TimeModule`'s in Stage-8, stays resolvable at the
        // title screen with no save loaded (`mach.ok` true, stale/boot-default field values) --
        // the machine row rendered "Fuel/Water Supply/Finished" labels with unresolved "-" values
        // over the pre-game art before this fix. Same guard discipline as every other Today field.
        const auto& mach = m.model.machines;
        const bool machines_ready = m.model.game_loaded && mach.ok;
        host->publish_i64(host->userdata, "mtas.machines_ok", machines_ready ? 1 : 0);
        host->publish_i64(host->userdata, "today.show_fallback_note",
                          m.model.game_loaded && missions.ok && !machines_ready ? 1 : 0);
        host->publish_i64(host->userdata, "machines.fuel_lack_good",
                          machines_ready && !rendered.machines_fuel_lack_bad ? 1 : 0);
        host->publish_i64(host->userdata, "machines.water_lack_good",
                          machines_ready && !rendered.machines_water_lack_bad ? 1 : 0);
        if (machines_ready) {
            host->publish_i64(host->userdata, "machines.fuel_lack_bad",
                              rendered.machines_fuel_lack_bad ? 1 : 0);
            host->publish_i64(host->userdata, "machines.water_lack_bad",
                              rendered.machines_water_lack_bad ? 1 : 0);
            host->publish_text(host->userdata, "machines.fuel_text",
                              rendered.machines_fuel_text.c_str());
            host->publish_text(host->userdata, "machines.water_text",
                              rendered.machines_water_text.c_str());
            host->publish_text(host->userdata, "machines.finished_text",
                              rendered.machines_finished_text.c_str());
            host->publish_text(host->userdata, "machines.working_text",
                              rendered.machines_working_text.c_str());
        }

        // ---- Social page + NPC card (Stage-10). SocialFields is the data lane's to fill
        // (met-NPCs-only, spoiler rule -- see mtas_reader.h); until `social.ok` is true this
        // publishes nothing and the page shows its native "haven't met anyone" empty state.
        // Selection (round-4 rewrite): each polaroid's tap is now a `"kind": "module"` action
        // ("select_npc", argument "$payload") -- see Module's own comment for why the previous
        // `select_group`/`@sel:` route could never work here, both generally (modules never see
        // "@sel:" at all) and specifically for a tap that also navigates (ResetInteraction's
        // page-change clear). `m.npc_sel` is this module's own selection state, set by
        // ActionCallback; navigation itself is a manifest `page_binds` entry watching
        // `npc_detail.nav_parity` (flips every select_npc call), not this widget's `on_tap` at
        // all. Round 8: a second, independent way into this same page now exists too -- a live
        // Dialog UI's own on-screen speaker (`dialogue_npc_sel`, computed above) overrides the
        // tap-driven `m.npc_sel` for this sample's rendering only; its own page_bind
        // (`dlg.active`, generator) handles the navigation, separately from `npc_detail.nav_parity`.
        host->publish_i64(host->userdata, "npc_detail.nav_parity", m.npc_nav_parity ? 1 : 0);
        const auto& social = m.model.social;
        host->publish_i64(host->userdata, "social.ok", social.ok && m.model.game_loaded ? 1 : 0);
        if (social.ok && m.model.game_loaded) {
            const auto order = SocialOrder(social, m.social_filter);
            const auto planning = SocialOrder(social, 0);
            for (std::size_t j = 0; j < 3; ++j) {
                const auto p = "plan" + std::to_string(j) + ".";
                host->publish_i64(host->userdata, (p + "v").c_str(), j < planning.size());
                if (j >= planning.size()) continue;
                const auto& n = social.npcs[planning[j]];
                const auto& rn = rendered.npcs[planning[j]];
                host->publish_text(host->userdata, (p + "name").c_str(), n.name.c_str());
                host->publish_text(host->userdata, (p + "card_name").c_str(), rn.card_name.c_str());
                PublishIconRender(host, p + "portrait", portrait_canvas ? rn.plan_portrait_alt : rn.portrait);
                host->publish_i64(host->userdata, (p + "birthday_ok").c_str(), rn.birthday_ok);
                host->publish_i64(host->userdata, (p + "gift_open").c_str(), !rn.birthday_ok && rn.gift_pending);
                host->publish_text(host->userdata, (p + "birthday_days").c_str(), std::to_string(n.days_until_birthday).c_str());
            }
            host->publish_i64(host->userdata, "plan.nav", m.plan_nav);

            const auto open = std::count_if(social.npcs.begin(), social.npcs.end(),
                [](const auto& n) { return n.gifted_ok && !n.gifted_today; });
            const bool gifts_known = social.npcs.size() == static_cast<std::size_t>(social.met_count) && std::all_of(social.npcs.begin(), social.npcs.end(),
                [](const auto& n) { return n.gifted_ok; });
            host->publish_i64(host->userdata, "social.gifts_known", gifts_known);
            host->publish_text(host->userdata, "social.gift_header",
                (Ui("not_gifted") + "  " + std::to_string(open) + " / " + std::to_string(social.met_count)).c_str());
            for (int f = 0; f < 4; ++f) {
                const auto p = "social.filter" + std::to_string(f);
                host->publish_i64(host->userdata, (p + ".selected").c_str(), f == m.social_filter);
                host->publish_i64(host->userdata, (p + ".idle").c_str(), f != m.social_filter);
            }
            host->publish_i64(host->userdata, "social.npc_count", order.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                for (const auto& p : std::array{std::string("npc") + std::to_string(i) + "."}) {
                    const auto raw = order[i];
                    const auto& rn = rendered.npcs[raw];
                    const auto& n = social.npcs[raw];
                    host->publish_i64(host->userdata, (p + "selected").c_str(), (m.npc_sel < 0 ? raw == order.front() : m.npc_sel == static_cast<std::int64_t>(raw)));
                    host->publish_text(host->userdata, (p + "id").c_str(), std::to_string(n.npc_id).c_str());
                    host->publish_text(host->userdata, (p + "birthday_days").c_str(), std::to_string(n.days_until_birthday).c_str());
                    host->publish_text(host->userdata, (p + "level_title").c_str(), n.level_title.c_str());
                    host->publish_i64(host->userdata, (p + "likes_count").c_str(), n.known_likes.size());
                    for (std::size_t j = 0; j < rn.known_likes.size(); ++j) {
                        const auto lp = p + "like" + std::to_string(j) + ".";
                        host->publish_text(host->userdata, (lp + "name").c_str(), rn.known_likes[j].name.c_str());
                        PublishIconRender(host, lp + "icon", rn.known_likes[j].icon);
                    }
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "name").c_str(), rn.name.c_str());
                    host->publish_text(host->userdata, (p + "card_name").c_str(), rn.card_name.c_str());
                    // Round-4: CONTAIN-fit (PublishPortrait), not a plain stretch (PublishIcon) -- see
                    // that helper's own comment. w,h must match social_page()'s own photo-slot rect
                    // exactly (316x148, the card's `cw-40` x `148`).
                    PublishIconRender(host, p + "portrait", rn.portrait);
                    // Round-3: bind hearts/hearts_max/stranger (DATA-CONTRACT.md "Social hearts"),
                    // not the raw `level` (round-2's own fix only hid a raw "-1"; the data lane has
                    // since added the real display-safe field the native UI itself uses -- a pip
                    // strip, never "Lv. N").
                    PublishHearts(host, p, rn.hearts);
                    // Chat/Gift: ok (the favor-action chain resolved at all) and today (done already
                    // this in-game day) kept as two separate binds -- not collapsed into one bool like
                    // Stage-9 originally published (dead, unwired) -- so the card can show a "done"
                    // check vs. an "open" ring instead of only ever an all-or-nothing tick.
                    host->publish_i64(host->userdata, (p + "chatted_ok").c_str(), rn.chatted_ok ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "chatted_today").c_str(),
                                      rn.chatted_today ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "chat_pending").c_str(),
                                      rn.chat_pending ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "gifted_ok").c_str(), rn.gifted_ok ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "gifted_today").c_str(),
                                      rn.gifted_today ? 1 : 0);
                    // Round-5: a third, POSITIVE "show the open ring" flag -- NOT `!gifted_today` in
                    // the manifest, which would be true (and draw a floating ring with no "Gift" word
                    // next to it, gift_lbl's own gate is `gifted_ok` alone) whenever `gifted_ok` is
                    // false too, since `gifted_today` is already 0 in that case (same "negating an
                    // always-published-but-sometimes-0 flag isn't the same as a per-row 'show' gate"
                    // class of bug this file's own Ingredients section found and fixed this round).
                    host->publish_i64(host->userdata, (p + "gift_pending").c_str(),
                                      rn.gift_pending ? 1 : 0);
                    // Round-3: the correct "upcoming birthday" gate is `0 <= days_until_birthday <= 6`
                    // (DATA-CONTRACT.md "Week events / birthday"), not `birthday_ok` (true for ~every
                    // met NPC with a known birthday, not just the ones within 7 days -- round-2's own
                    // `BirthdaySoon` re-derived this same window by hand before the data lane added
                    // the field properly; now published directly, no local recomputation needed).
                    host->publish_i64(host->userdata, (p + "birthday_ok").c_str(),
                                      rn.birthday_ok ? 1 : 0);
                    if (rn.birthday_ok)
                        host->publish_text(host->userdata, (p + "birthday_text").c_str(),
                                          rn.birthday_text.c_str());
                }
            }
            // Round 8: a live dialogue with a known NPC takes over the detail page's own
            // selection, without touching `m.npc_sel` itself (the player's own last manual
            // tap) -- so browsing Social normally again after the dialogue ends still shows
            // whatever they tapped before, not whoever last spoke.
            const auto npc_sel = dialogue_npc_sel >= 0 ? dialogue_npc_sel :
                (m.npc_sel >= 0 ? m.npc_sel : (order.empty() ? -1 : static_cast<std::int64_t>(order.front())));
            const bool have_sel =
                npc_sel >= 0 && static_cast<std::size_t>(npc_sel) < social.npcs.size();
            host->publish_i64(host->userdata, "npc_detail.ok", have_sel ? 1 : 0);
            if (have_sel)
                host->publish_text(host->userdata, "npc_detail.name",
                                  rendered.npcs[static_cast<std::size_t>(npc_sel)].name.c_str());
            // PublishHearts/portrait/birthday called UNCONDITIONALLY (every sample, not only
            // inside `if (have_sel)`) against the stable, cached "no selection" render (built from
            // a default-constructed NpcFields{} -- see BuildRendered's own `no_selection_npc`)
            // when nothing is selected -- every one of its fields (hearts_max=0, portrait="",
            // days_until_birthday=-1) already means "show nothing" to those two helpers, so this
            // is always fresh and never a stale "true" left over from a previously selected NPC
            // once the player backs out to the grid (the `need_bind` grammar only takes one point
            // name, no "A and B" expression, so each row's own gate has to already be correct on
            // its own rather than relying on an additional "npc_detail.ok" check in the manifest).
            const auto& drn =
                have_sel ? rendered.npcs[static_cast<std::size_t>(npc_sel)] : rendered.no_selection_npc;
            PublishHearts(host, "npc_detail.", drn.hearts);
            // Round-4: CONTAIN-fit, AND a real bug fix -- this used to request 400x480 against
            // npc_page()'s own 400x340 portrait rect (a second, independent stretch: the engine's
            // DrawImage always scales whatever buffer comes back to the widget's rect with no
            // aspect handling of its own, see PublishPortrait's comment), silently squashing the
            // portrait vertically by a further ~0.71x on top of the native-aspect mismatch. Now
            // requests the rect's own real size. Unlike the grid card's portrait (cached above,
            // 316x148), the detail rect is a DIFFERENT size (400x340) -- only one npc is ever
            // selected at a time, so this one is cheap to rebuild fresh every tick rather than
            // caching a second, differently-sized portrait per npc for a slot that is only ever
            // shown once.
            const std::string detail_portrait_name =
                have_sel ? social.npcs[static_cast<std::size_t>(npc_sel)].portrait
                         : std::string{};
            PublishIconRender(host, "npc_detail.portrait",
                              RenderPortrait(detail_portrait_name, 400, 340));
            host->publish_i64(host->userdata, "npc_detail.birthday_ok", drn.birthday_ok ? 1 : 0);
            // Round-5: Chat/Gift row (dir-b-npc.png), same fields/discipline as the grid card --
            // was published for npc{i}.* only through round 4, never for the detail page. `drn` is
            // already the UNCONDITIONAL-every-sample default-or-selected render the hearts/
            // portrait/birthday calls above use, so this inherits the same "never stale once
            // backed out" guarantee for free.
            host->publish_i64(host->userdata, "npc_detail.chatted_ok", drn.chatted_ok ? 1 : 0);
            host->publish_i64(host->userdata, "npc_detail.chatted_today",
                              drn.chatted_today ? 1 : 0);
            host->publish_i64(host->userdata, "npc_detail.chat_pending", drn.chat_pending ? 1 : 0);
            host->publish_i64(host->userdata, "npc_detail.gifted_ok", drn.gifted_ok ? 1 : 0);
            host->publish_i64(host->userdata, "npc_detail.gifted_today", drn.gifted_today ? 1 : 0);
            host->publish_i64(host->userdata, "npc_detail.gift_pending", drn.gift_pending ? 1 : 0);
            const NpcFields* selected = have_sel ? &social.npcs[static_cast<std::size_t>(npc_sel)] : nullptr;
            const auto say = [&](const std::string& key, const std::string& value) {
                host->publish_text(host->userdata, key.c_str(), value.c_str());
            };
            say("npc_detail.level_title", selected ? selected->level_title : "");
            host->publish_i64(host->userdata, "npc_detail.title_ok", selected && !selected->level_title.empty());
            host->publish_i64(host->userdata, "npc_detail.birthday_known", selected && selected->birthday_ok);
            if (selected) {
                const auto& n = *selected;
                say("npc_detail.birthday_date", UiFormat(Ui("birthday_date"), {std::to_string(n.birthday_day), Ui(std::to_string(1283 + n.birthday_month))}));
                say("npc_detail.birthday_countdown", n.days_until_birthday >= 0
                    ? UiFormat(Ui("in_days"), {std::to_string(n.days_until_birthday)}) : "");
                say("npc_detail.next_points", std::to_string(n.next_level_points));
                PublishIconRender(host, "npc_detail.social_portrait", drn.social_portrait);
                PublishIconRender(host, "npc_detail.dialogue_portrait", portrait_canvas ? drn.dialogue_portrait_alt : drn.dialogue_portrait);
            }
            say("npc_detail.chat_status", drn.chatted_today ? Ui("chatted") : Ui("not_yet"));
            say("npc_detail.gift_status", drn.gifted_today ? Ui("gifted") : Ui("not_yet"));
            for (int j = 0; j < 7; ++j) {
                const auto p = "npc_detail.pip" + std::to_string(j);
                host->publish_i64(host->userdata, (p + ".v").c_str(), selected && j < selected->hearts_max);
                say(p + ".src", std::string("module:mtas:visible:") +
                    (selected && selected->social_type == 4 ? (j < selected->hearts ? "I_Social_img_Paw_00" : "I_Social_img_Paw_01") :
                    (selected && j < selected->hearts ? "SocialHeartOn" : "SocialHeartOff")) + ":48:48");
            }
            host->publish_i64(host->userdata, "npc_detail.likes_count", drn.known_likes.size());
            host->publish_i64(host->userdata, "npc_detail.like_slots_count", have_sel ? std::max(std::size_t{8}, drn.known_likes.size()) : 0);
            for (std::size_t i = 0; i < std::max(std::size_t{8}, drn.known_likes.size()); ++i) {
                const auto p = "npc_like" + std::to_string(i) + ".";
                const bool known = selected && i < drn.known_likes.size();
                host->publish_i64(host->userdata, (p + "v").c_str(), have_sel);
                host->publish_i64(host->userdata, (p + "known").c_str(), known);
                host->publish_i64(host->userdata, (p + "owned_ok").c_str(), known && m.model.inventory.ok);
                host->publish_i64(host->userdata, (p + "unknown").c_str(), have_sel && !known);
                if (!known) continue;
                const auto& like = drn.known_likes[i];
                const auto id = selected->known_likes[i];
                std::int64_t owned = 0;
                for (const auto& it : m.model.inventory.items) if (it.item_id == id) owned += it.count;
                say(p + "name", like.name);
                say(p + "owned", std::to_string(owned));
                host->publish_i64(host->userdata, (p + "none_owned").c_str(), m.model.inventory.ok && owned == 0);
                const bool excellent = std::find(selected->excellent_likes.begin(), selected->excellent_likes.end(), id) != selected->excellent_likes.end();
                say(p + "tier_src", std::string("module:mtas:ui:I_Backpack_icon_") + (excellent ? "GiftVerylike_00" : "GiftLike_00") + ":40:40");
                PublishIconRender(host, p + "icon", like.icon);
            }
        }

        // ---- Inventory + Ingredients (Stage-10). InventoryFields is the data lane's to fill
        // (bag + storage, merged); until `inv.ok` is true the grid shows its native "bag empty"
        // state. Source filter (All/Bag/Storage) and cell selection (round-4 rewrite) are both
        // `"kind": "module"` actions (`set_source`/`select_item`) into `m.source`/`m.item_sel` --
        // see Module's own comment for why the old `select_group`/`@sel:` route never actually
        // worked (a module's own sample() can never see "@sel:" values at all, any tick, found
        // live this round) -- the module re-filters/re-publishes the whole visible slice every
        // sample rather than the manifest trying to hide/show pre-published rows itself (there is
        // no per-row "matches current filter" gate primitive).
        const auto& inv = m.model.inventory;
        host->publish_i64(host->userdata, "inv.ok", inv.ok && m.model.game_loaded ? 1 : 0);
        host->publish_i64(host->userdata, "inv.empty", 0);
        host->publish_i64(host->userdata, "item_detail.ok", 0);
        host->publish_i64(host->userdata, "item_detail.empty", 1);
        host->publish_i64(host->userdata, "item_detail.icon_ok", 0);
        host->publish_i64(host->userdata, "item_detail.description_ok", 0);
        host->publish_i64(host->userdata, "item_detail.category_ok", 0);
        const bool categories_ok = m.model.game_loaded && inv.ok && std::all_of(inv.items.begin(), inv.items.end(),
            [](const InvItem& it) { return it.category_ok; });
        host->publish_i64(host->userdata, "inv.categories_ok", categories_ok);
        for (const int filter : {0, 1, 2, 3, 4, 5, 6, 9}) {
            const auto prefix = "inv.category" + std::to_string(filter);
            const bool selected = m.category == filter;
            host->publish_i64(host->userdata, (prefix + ".selected").c_str(), categories_ok && selected);
            host->publish_i64(host->userdata, (prefix + ".idle").c_str(), categories_ok && !selected);
        }
        host->publish_i64(host->userdata, "item_detail.liked_count", 0);
        host->publish_i64(host->userdata, "item_detail.needed_count", 0);
        if (inv.ok && m.model.game_loaded) {
            // Round-4: gates the Storage filter pill itself (source_toggle()) -- before this
            // lands, every item's own `in_storage` is always false (InventoryFields.storage_ok
            // doc comment), so a player tapping "Storage" would see a confusingly-empty grid with
            // no explanation; hiding the pill until there is real storage data to filter is the
            // same "never publish a dead end" discipline as every other `_ok` gate here.
            host->publish_i64(host->userdata, "inv.storage_ok", inv.storage_ok ? 1 : 0);
            host->publish_text(host->userdata, "inv.bag_text", rendered.bag_text.c_str());
            const auto source = m.source; // 0 all / 1 bag / 2 storage
            for (int i = 0; i < 3; ++i)
                host->publish_i64(host->userdata, ("inv.source" + std::to_string(i)).c_str(), source == i);
            // Filtering reads the RAW model item every tick (cheap: a couple of field compares,
            // no formatting) -- only the per-entry TEXT below (formatted once per refresh by
            // BuildRendered) is looked up by original index.
            std::vector<const InvItem*> filtered;
            filtered.reserve(inv.items.size());
            for (const auto& it : inv.items) {
                if (source == 1 && it.in_storage)
                    continue;
                if (source == 2 && !it.in_storage)
                    continue;
                if (!MatchesInventoryCategory(it, categories_ok ? m.category : 0))
                    continue;
                filtered.push_back(&it);
            }
            host->publish_i64(host->userdata, "inv.item_count",
                              static_cast<std::int64_t>(filtered.size()));
            host->publish_text(host->userdata, "inv.filtered_text",
                              Grouped(static_cast<std::int64_t>(filtered.size())).c_str());
            host->publish_i64(host->userdata, "inv.empty", filtered.empty());
            host->publish_text(host->userdata, "inv.filtered_label",
                              (Ui("52005") + ": " + std::to_string(filtered.size())).c_str());
            const auto stored = std::count_if(inv.items.begin(), inv.items.end(), [](const auto& it) { return it.in_storage; });
            host->publish_text(host->userdata, "inv.storage_count_text", std::to_string(stored).c_str());
            for (std::size_t i = 0; i < filtered.size(); ++i) {
                for (const auto& p : std::array{std::string("item") + std::to_string(i) + "."}) {
                    const auto& it = *filtered[i];
                    const auto idx = static_cast<std::size_t>(filtered[i] - inv.items.data());
                    const auto& ri = rendered.items[idx];
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "name").c_str(), ri.name.c_str());
                    host->publish_text(host->userdata, (p + "count_text").c_str(),
                                      ri.count_text.c_str());
                    host->publish_i64(host->userdata, (p + "selected").c_str(), static_cast<std::int64_t>(i) == m.item_sel);
                    host->publish_i64(host->userdata, (p + "in_storage").c_str(),
                                      it.in_storage ? 1 : 0);
                    PublishIconRender(host, p + "icon", ri.icon);
                }
            }
            host->publish_text(host->userdata, "inv.summary",
                (rendered.bag_label + ": " + rendered.bag_text + "\n" +
                 rendered.storage_label + ": " + std::to_string(stored)).c_str());
            const auto item_sel = m.item_sel;
            const bool have_sel = item_sel >= 0 &&
                                  static_cast<std::size_t>(item_sel) < filtered.size();
            host->publish_i64(host->userdata, "item_detail.ok", have_sel ? 1 : 0);
            host->publish_i64(host->userdata, "item_detail.empty", !have_sel);
            if (have_sel) {
                const auto& it = *filtered[static_cast<std::size_t>(item_sel)];
                const auto idx = static_cast<std::size_t>(
                    filtered[static_cast<std::size_t>(item_sel)] - inv.items.data());
                const auto& ri = rendered.items[idx];
                host->publish_text(host->userdata, "item_detail.name", ri.name.c_str());
                std::int64_t bag_count = 0, storage_count = 0;
                for (const auto& stack : inv.items) {
                    if (stack.item_id == it.item_id) {
                        (stack.in_storage ? storage_count : bag_count) += stack.count;
                    }
                }
                const auto counts = rendered.bag_label + ": " + std::to_string(bag_count) +
                    (inv.storage_ok ? "\n" + rendered.storage_label + ": " + std::to_string(storage_count) : "");
                host->publish_text(host->userdata, "item_detail.count_text", counts.c_str());
                host->publish_text(host->userdata, "item_detail.bag_count", std::to_string(bag_count).c_str());
                host->publish_text(host->userdata, "item_detail.storage_count", std::to_string(storage_count).c_str());
                host->publish_text(host->userdata, "item_detail.quality", QualityText(m.model, it.grade, 0, true).c_str());
                std::size_t liked = 0;
                for (const auto& n : m.model.social.npcs) {
                    if (std::find(n.known_likes.begin(), n.known_likes.end(), it.item_id) == n.known_likes.end()) continue;
                    const auto p = "item_liked" + std::to_string(liked++) + ".";
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "name").c_str(), n.name.c_str());
                    PublishIconRender(host, p + "portrait", RenderPortrait(n.portrait, 64, 64));
                }
                host->publish_i64(host->userdata, "item_detail.liked_count", liked);
                const auto uses = NeededFor(m.model, it.item_id);
                host->publish_i64(host->userdata, "item_detail.needed_count", uses.size());
                for (std::size_t j = 0; j < uses.size(); ++j) {
                    const auto p = "item_needed" + std::to_string(j) + ".";
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "name").c_str(), uses[j].name.c_str());
                    const auto have = uses[j].progress >= 0 ? uses[j].progress : bag_count + storage_count;
                    host->publish_text(host->userdata, (p + "count").c_str(),
                        (std::to_string(have) + "/" + std::to_string(uses[j].need)).c_str());
                    host->publish_i64(host->userdata, (p + "enough").c_str(), have >= uses[j].need);
                    host->publish_i64(host->userdata, (p + "short").c_str(), have < uses[j].need);
                    host->publish_text(host->userdata, (p + "icon_src").c_str(), uses[j].mission ?
                        "module:mtas:ui:MissionIconOther:48:48" : "module:mtas:ui:TabHandbookNor:48:48");
                }

                std::string category;
                for (const auto [filter, message] : {std::pair{2, 52002}, {3, 52003},
                        {4, 52004}, {5, 52001}, {6, 52000}, {9, 52009}, {1, 52008}}) {
                    if (MatchesInventoryCategory(it, filter)) { category = GameText().Get(message); break; }
                }
                if (category.empty() && it.category_ok) category = GameText().Get(1703);
                host->publish_i64(host->userdata, "item_detail.id", it.item_id);
                host->publish_text(host->userdata, "item_detail.category", category.c_str());
                host->publish_i64(host->userdata, "item_detail.category_ok", !category.empty());
                const auto description = GameText().ItemDescription(it.item_id);
                host->publish_text(host->userdata, "item_detail.description", description.c_str());
                host->publish_i64(host->userdata, "item_detail.description_ok", !description.empty());
                host->publish_text(host->userdata, "item_detail.source_text",
                                  (it.in_storage ? rendered.storage_label : rendered.bag_label)
                                      .c_str());
                // item_detail's own 96x96 icon is a DIFFERENT size than the grid card's cached
                // 64x64 one (ri.icon) -- same "one selected slot, cheap to rebuild fresh" call as
                // npc_detail's own differently-sized portrait, above.
                PublishIconRender(host, "item_detail.icon", RenderIcon(it.icon, 96, 96));
            }
        }

        // ---- Ingredients (Stage-10 sub-view, round-4: wired now that the data lane's
        // IngredientsFields landed). Reached via the Inventory page's own "Ingredients" button;
        // until `ing.ok` the page shows its native "not available" empty state, same discipline
        // as every other Stage-9-contract block.
        const auto& ing = m.model.ingredients;
        host->publish_i64(host->userdata, "ing.ok", ing.ok && m.model.game_loaded ? 1 : 0);
        if (ing.ok && m.model.game_loaded) {
            host->publish_i64(host->userdata, "ing.count",
                              static_cast<std::int64_t>(ing.known.size()));
            host->publish_text(host->userdata, "ing.can_cook_text",
                              rendered.ing_can_cook_text.c_str());
            const auto selected = std::min(static_cast<std::size_t>(m.recipe_sel), ing.known.empty() ? 0 : ing.known.size() - 1);
            host->publish_i64(host->userdata, "recipe_detail.ok", !ing.known.empty());
            if (!ing.known.empty()) {
                const auto& r = ing.known[selected];
                host->publish_i64(host->userdata, "recipe_detail.ready", r.can_cook_now);
                host->publish_i64(host->userdata, "recipe_detail.missing", !r.can_cook_now);
                host->publish_text(host->userdata, "recipe_detail.name", r.dish_name.c_str());
                PublishIconRender(host, "recipe_detail.icon", rendered.recipes[selected].icon);
                host->publish_text(host->userdata, "recipe_detail.status", r.can_cook_now ? Ui("ready").c_str() : rendered.recipes[selected].need_list_text.c_str());
                host->publish_i64(host->userdata, "recipe_detail.material_count", r.materials.size());
                for (std::size_t j = 0; j < r.materials.size(); ++j) {
                    const auto p = "recipe_mat" + std::to_string(j);
                    PublishMaterial(host, p, rendered.recipes[selected].detail_mats[j], "count");
                    host->publish_text(host->userdata, (p + ".name").c_str(), r.materials[j].item_name.c_str());
                }
            }
            for (std::size_t i = 0; i < ing.known.size(); ++i) {
                host->publish_i64(host->userdata, ("recipe" + std::to_string(i) + ".selected").c_str(), i == selected);
                for (const auto& p : std::array{std::string("recipe") + std::to_string(i) + "."}) {
                    const auto& r = ing.known[i];
                    const auto& rr = rendered.recipes[i];
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_text(host->userdata, (p + "name").c_str(), rr.name.c_str());
                    PublishIconRender(host, p + "icon", rr.icon);
                    host->publish_i64(host->userdata, (p + "can_cook").c_str(),
                                      r.can_cook_now ? 1 : 0);
                    // Round-5 polish: dir-b-ingredients.png's own "Need: Lungfish" line (the Sweet and
                    // Sour Sandfish card) -- every missing material's name, comma-joined; published
                    // only when at least one is actually missing (never an empty "Need:" label).
                    if (rr.need_list_ok) {
                        host->publish_i64(host->userdata, (p + "need_list_ok").c_str(), 1);
                        host->publish_text(host->userdata, (p + "need_list_text").c_str(),
                                          rr.need_list_text.c_str());
                    }
                    for (std::size_t j = 0; j < IngredientsMatCap; ++j)
                        PublishMaterial(host, p + "mat" + std::to_string(j), rr.mats[j], "have_text");
                }
            }
        }

        // Catalogue slots retain native order; locked cards never publish item details.
        const auto& hb = m.model.handbook;
        host->publish_i64(host->userdata, "hb.ok", hb.ok && m.model.game_loaded ? 1 : 0);
        if (hb.ok && m.model.game_loaded) {
            // Filtering reads the RAW model slot's `category` every tick (cheap compare, no
            // formatting); only the per-slot TEXT below (formatted once per refresh) comes from
            // the cache, by original slot index.
            std::vector<const BlueprintFields*> filtered;
            for (const auto& b : hb.slots) {
                if (m.handbook_category == 0 || b.category == m.handbook_category - 1)
                    filtered.push_back(&b);
            }
            const auto selected = std::min(static_cast<std::size_t>(m.blueprint_sel), filtered.empty() ? 0 : filtered.size() - 1);
            host->publish_i64(host->userdata, "blueprint_detail.ok", !filtered.empty() && filtered[selected]->unlocked);
            if (!filtered.empty() && filtered[selected]->unlocked) {
                const auto& b = *filtered[selected];
                host->publish_text(host->userdata, "blueprint_detail.name", b.item_name.c_str());
                PublishIconRender(host, "blueprint_detail.icon", rendered.blueprints[static_cast<std::size_t>(filtered[selected] - hb.slots.data())].icon);
                host->publish_i64(host->userdata, "blueprint_detail.material_count", b.materials.size());
                for (std::size_t j = 0; j < b.materials.size(); ++j) {
                    const auto p = "blueprint_mat" + std::to_string(j);
                    PublishMaterial(host, p, rendered.blueprints[static_cast<std::size_t>(filtered[selected] - hb.slots.data())].detail_mats[j], "count");
                    host->publish_text(host->userdata, (p + ".name").c_str(), b.materials[j].item_name.c_str());
                }
            }
            host->publish_i64(host->userdata, "hb.total", hb.slots.size());
            for (int category = 0; category <= 2; ++category) {
                const auto count = category == 0 ? hb.slots.size() :
                    static_cast<std::size_t>(std::count_if(hb.slots.begin(), hb.slots.end(),
                        [category](const BlueprintFields& b) { return b.category == category - 1; }));
                const auto prefix = "hb.category" + std::to_string(category);
                host->publish_i64(host->userdata, (prefix + ".count").c_str(), count);
                host->publish_i64(host->userdata, (prefix + ".selected").c_str(),
                                  m.handbook_category == category);
                host->publish_i64(host->userdata, (prefix + ".idle").c_str(),
                                  m.handbook_category != category);
                host->publish_text(host->userdata, (prefix + ".text").c_str(),
                                  rendered.hb_category_text[category].c_str());
            }
            host->publish_i64(host->userdata, "hb.count", filtered.size());
            for (std::size_t i = 0; i < filtered.size(); ++i) {
                for (const auto& p : std::array{std::string("bp") + std::to_string(i) + "."}) {
                    const auto idx = static_cast<std::size_t>(filtered[i] - hb.slots.data());
                    const auto& rb = rendered.blueprints[idx];
                    host->publish_i64(host->userdata, (p + "selected").c_str(), i == selected);
                    host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                    host->publish_i64(host->userdata, (p + "unlocked").c_str(), rb.unlocked);
                    host->publish_i64(host->userdata, (p + "locked").c_str(), !rb.unlocked);
                    if (!rb.unlocked) {
                        host->publish_text(host->userdata, (p + "name").c_str(), "???");
                        PublishIconRender(host, p + "icon", IconRender{});
                        host->publish_i64(host->userdata, (p + "fresh").c_str(), 0);
                        host->publish_i64(host->userdata, (p + "overflow_green").c_str(), 0);
                        host->publish_i64(host->userdata, (p + "overflow_red").c_str(), 0);
                        for (std::size_t j = 0; j < HandbookMatCap; ++j) {
                            const auto mat = p + "mat" + std::to_string(j);
                            PublishMaterial(host, mat, rb.mats[j], "frac_text");
                            for (const char* flag : {".icon_ok", ".need_ok_green", ".need_ok_red"})
                                host->publish_i64(host->userdata, (mat + flag).c_str(), 0);
                        }
                        continue;
                    }
                    host->publish_text(host->userdata, (p + "name").c_str(), rb.name.c_str());
                    PublishIconRender(host, p + "icon", rb.icon);
                    host->publish_i64(host->userdata, (p + "fresh").c_str(), rb.fresh ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "overflow_green").c_str(),
                                      rb.overflow_green ? 1 : 0);
                    host->publish_i64(host->userdata, (p + "overflow_red").c_str(),
                                      rb.overflow_red ? 1 : 0);
                    if (rb.overflow_ok)
                        host->publish_text(host->userdata, (p + "overflow_text").c_str(),
                                          rb.overflow_text.c_str());
                    for (std::size_t j = 0; j < HandbookMatCap; ++j)
                        PublishMaterial(host, p + "mat" + std::to_string(j), rb.mats[j], "frac_text");
                }
            }
        }

        // ---- Workshop (rank hero + ranking) + Player level/EXP (Stage-10). WorkshopFields/
        // PlayerFields are the data lane's to fill; until `ws.ok`/`player.ok` are true the page
        // shows its native "not available" state for each half independently (the hero card and
        // the ranking paper gate on the same `ws.ok`, Player's own level/EXP row gates separately
        // since it is a different native source, see mtas_reader.h).
        const auto& ws = m.model.workshop;
        host->publish_i64(host->userdata, "ws.ok", ws.ok && m.model.game_loaded ? 1 : 0);
        if (ws.ok && m.model.game_loaded) {
            host->publish_text(host->userdata, "ws.rep_needed", rendered.ws_rep_needed.c_str());
            host->publish_text(host->userdata, "ws.rank_text", rendered.ws_rank_text.c_str());
            // Guard rail (Stage-10, found live): WorkshopFields.rep_value/rep_next is still the
            // data lane's own field to finish, and an implausible value (observed live: a
            // 40+-digit FracText string, which fit_text/wrap_width could not contain -- a label
            // over its own natural width THAT far beyond its box apparently still mis-anchors on
            // this host, filed as a UI-lane open issue rather than chased further) got published
            // and rendered off-canvas. Rather than trust an unvalidated field, only publish the
            // rep bar/text when both values are in Sandrock's actual plausible range (rep totals
            // are observed in the low thousands for a rank-3 save) -- `ws.rep_ok` gates the bar +
            // label in the manifest; `ws.rank_text`/the ranking table (verified sane every sample
            // this pass) are unaffected.
            host->publish_i64(host->userdata, "ws.rep_ok", rendered.ws_rep_ok ? 1 : 0);
            if (rendered.ws_rep_ok) {
                host->publish_text(host->userdata, "ws.rep_text", rendered.ws_rep_text.c_str());
                host->publish_i64(host->userdata, "ws.rep_value", ws.rep_value);
                host->publish_i64(host->userdata, "ws.rep_next", ws.rep_next);
            }
            host->publish_i64(host->userdata, "ws.rank_count",
                              static_cast<std::int64_t>(std::min(ws.ranking.size(), WorkshopRankCap)));
            for (std::size_t i = 0; i < WorkshopRankCap; ++i) {
                const std::string p = "rank" + std::to_string(i) + ".";
                if (i >= ws.ranking.size())
                    continue;
                const auto& r = ws.ranking[i];
                const auto& rr = rendered.ranks[i];
                host->publish_i64(host->userdata, (p + "v").c_str(), 1);
                // GuildRankingUI.AddRanking (0x2988918): rankColor[index] for the first
                // three rows; the prefab's disabled rankBg stays hidden for later rows.
                host->publish_i64(host->userdata, (p + "badge").c_str(), rr.badge);
                host->publish_i64(host->userdata, (p + "plain").c_str(), !rr.badge);
                host->publish_text(host->userdata, (p + "badge_src").c_str(), rr.badge_src.c_str());
                host->publish_text(host->userdata, (p + "pos_text").c_str(), rr.pos_text.c_str());
                host->publish_text(host->userdata, (p + "name").c_str(), rr.name.c_str());
                host->publish_text(host->userdata, (p + "value_text").c_str(),
                                  rr.value_text.c_str());
                host->publish_i64(host->userdata, (p + "is_player").c_str(), r.is_player ? 1 : 0);
                host->publish_i64(host->userdata, (p + "is_other").c_str(), !r.is_player);
            }
        }
        const auto& player = m.model.player;
        host->publish_i64(host->userdata, "player.ok", player.ok && m.model.game_loaded ? 1 : 0);
        if (player.ok && m.model.game_loaded) {
            host->publish_text(host->userdata, "player.level_text",
                              rendered.player_level_text.c_str());
            // Round-4: published unconditionally (not just inside `if (player.exp_ok)`) so the
            // manifest's EXP bar/text can gate on this specific flag instead of the coarser
            // `player.ok` (level can resolve while EXP has not yet, PlayerFields' own doc
            // comment) -- the workshop EXP bar/text used to share `player.ok` and could draw an
            // empty/zero bar before EXP itself was ever proven live.
            host->publish_i64(host->userdata, "player.exp_ok", player.exp_ok ? 1 : 0);
            if (player.exp_ok) {
                host->publish_i64(host->userdata, "player.exp", player.exp);
                host->publish_i64(host->userdata, "player.exp_next",
                                  player.exp_next > 0 ? player.exp_next : 1);
                host->publish_text(host->userdata, "player.exp_text",
                                  rendered.player_exp_text.c_str());
            }
        }
    } catch (...) {
        if (host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "My Time at Sandrock sample failed");
    }
}

void TickCallback(void* p, const EdenDsmodHostApi* host) {
    if (!p || !host)
        return;
    auto& m = *static_cast<Module*>(p);
    m.settings = ReadSettings(*host);
    if (!m.helper.store_u32)
        return;
    const auto& settings = m.settings;
    const bool shown = host->get_i64 && host->get_i64(host->userdata, "__aux_present", 0) != 0;
    const bool gaming = m.model.game_loaded && m.model.ui.ok && m.model.ui.state_type == 0;
    const std::uint32_t mask = shown && gaming ?
        (settings.clock_top ? 0u : 1u) | (settings.tracker_top ? 0u : 2u) : 0u;
    m.helper.store_u32(m.helper.userdata, 12, mask);
    // The guest restores owned objects after 120 frames without a renewal.
    m.helper.store_u32(m.helper.userdata, 8, mask ? 120 : 0);
}

void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    auto* m = static_cast<Module*>(p);
    if (!m || !ext || ext->version != EDEN_DSMOD_EXT_VERSION ||
        ext->struct_size != sizeof(*ext) || ext->abi_hash != EDEN_DSMOD_EXT_HASH)
        return;
    if (ext->mailbox_address && ext->mailbox_size >= 0x1000 && ext->store_u32 && ext->load_u32)
        m->helper = *ext;
    if (m->art && ext->decode_astc)
        m->art->SetAstcDecoder(ce_unity::MakeHostAstcDecoder(ext->userdata, ext->decode_astc));
}

void ConfigureSaveCallback(void* p, const EdenDsmodHostSaveApi* api) {
    auto* m = static_cast<Module*>(p);
    if (!m || !api || api->version != EDEN_DSMOD_SAVE_EXT_VERSION ||
        api->struct_size < sizeof(*api) || api->abi_hash != EDEN_DSMOD_SAVE_EXT_HASH ||
        !api->read_save_file)
        return;
    m->save_api = *api;
    m->have_save_api = true;
}

/// Round-4 polish: selection/filter actions only (still no guest writes -- nothing here ever
/// touches game memory, only the module's own local UI-selection state, see Module's own comment
/// for why `@sel:`/`select_group` can't carry this instead). `select_npc`/`select_item` take the
/// tapped row's index as `arg` ("$payload" in the manifest action, the repeat template's own "{i}"
/// substitution); `set_source` takes a literal 0/1/2 (three distinct non-templated widgets, one
/// action each, same convention Pokémon Sword's own `battle_view`/`dex_region` actions use for a
/// small fixed set of buttons). Any other action name still declines (EDEN_DSMOD_FALSE), matching
/// the prior stub's "read-only" contract for everything except this new, explicitly-named set.
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t arg) {
    if (!p || !action)
        return EDEN_DSMOD_FALSE;
    auto& m = *static_cast<Module*>(p);
    const std::string_view a{action};
    if (m.loading)
        return EDEN_DSMOD_FALSE;
    if (a == "select_npc") {
        const auto order = SocialOrder(m.model.social, m.social_filter);
        if (arg < 0 || static_cast<std::size_t>(arg) >= order.size()) return EDEN_DSMOD_FALSE;
        m.npc_sel = static_cast<std::int64_t>(order[static_cast<std::size_t>(arg)]);
        return EDEN_DSMOD_TRUE;
    }
    if (a == "set_social_filter" || a == "set_mission_filter") {
        if (arg < 0 || arg > 3) return EDEN_DSMOD_FALSE;
        (a == "set_social_filter" ? m.social_filter : m.mission_filter) = static_cast<s32>(arg);
        if (a == "set_mission_filter") m.mission_sel = 0;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "select_mission" || a == "select_recipe" || a == "select_blueprint") {
        if (arg < 0) return EDEN_DSMOD_FALSE;
        (a == "select_mission" ? m.mission_sel : a == "select_recipe" ? m.recipe_sel : m.blueprint_sel) = arg;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "plan_npc" || a == "day_npc") {
        const auto order = SocialOrder(m.model.social, 0);
        if (a == "plan_npc") {
            if (arg < 0 || static_cast<std::size_t>(arg) >= order.size()) return EDEN_DSMOD_FALSE;
            m.npc_sel = order[static_cast<std::size_t>(arg)];
        } else {
            const auto it = std::find_if(m.model.social.npcs.begin(), m.model.social.npcs.end(),
                [arg](const auto& n) { return n.days_until_birthday == arg; });
            if (it == m.model.social.npcs.end()) return EDEN_DSMOD_FALSE;
            m.npc_sel = it - m.model.social.npcs.begin();
        }
        m.social_filter = 0;
        m.plan_nav = !m.plan_nav;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "tracked_mission") {
        const auto sorted = SortedMissions(m.model.missions);
        const auto it = std::find_if(sorted.begin(), sorted.end(), [&](const auto* mission) { return mission->title == m.model.tracked.title; });
        if (it == sorted.end()) return EDEN_DSMOD_FALSE;
        m.mission_filter = 0;
        m.mission_sel = it - sorted.begin();
        m.npc_nav_parity = !m.npc_nav_parity;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "select_item") {
        m.item_sel = arg;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "set_source") {
        if (arg < 0 || arg > 2)
            return EDEN_DSMOD_FALSE;
        m.source = arg;
        m.item_sel = -1;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "set_handbook_category") {
        if (arg < 0 || arg > 2)
            return EDEN_DSMOD_FALSE;
        m.handbook_category = static_cast<s32>(arg);
        m.blueprint_sel = 0;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "set_category") {
        if (arg < 0 || (arg > 6 && arg != 9))
            return EDEN_DSMOD_FALSE;
        m.category = static_cast<s32>(arg);
        m.item_sel = -1;
        return EDEN_DSMOD_TRUE;
    }
    return EDEN_DSMOD_FALSE;
}

/// "<name>:<w>:<h>" (already past a "module:mtas:<kind>:" prefix) -> name/w/h, or nullopt on any
/// malformed/out-of-range piece. Shared by every `module:mtas:*` key LoadImageCallback parses.
std::optional<std::tuple<std::string_view, std::uint32_t, std::uint32_t>> ParseNameWH(
    std::string_view rest) {
    const auto c1 = rest.find(':');
    if (c1 == std::string_view::npos)
        return std::nullopt;
    const auto c2 = rest.find(':', c1 + 1);
    if (c2 == std::string_view::npos)
        return std::nullopt;
    const std::string_view name = rest.substr(0, c1);
    std::uint32_t w = 0, h = 0;
    for (const char c : rest.substr(c1 + 1, c2 - c1 - 1)) {
        if (c < '0' || c > '9')
            return std::nullopt;
        w = w * 10 + static_cast<std::uint32_t>(c - '0');
        if (w > 4096)
            return std::nullopt;
    }
    for (const char c : rest.substr(c2 + 1)) {
        if (c < '0' || c > '9')
            return std::nullopt;
        h = h * 10 + static_cast<std::uint32_t>(c - '0');
        if (h > 4096)
            return std::nullopt;
    }
    // Match the host's per-image limit before allocating the scaled RGBA buffer.
    if (w == 0 || h == 0 || std::uint64_t{w} * h * 4 > 16u * 1024u * 1024u)
        return std::nullopt;
    return std::make_tuple(name, w, h);
}

/// Round-4 polish (orchestrator review item 1, "fit the sprite's full rect into the photo slot,
/// keep aspect, no stretch"): scales `src` down (nearest-neighbour, matching mtas_art.cpp's own
/// ScaleRect quality level -- no new quality bar to clear) to the largest size that fits inside
/// `out_w`x`out_h` without changing its aspect ratio, centres it on both axes, and leaves the rest
/// of the output buffer fully transparent. Unlike Art::LoadSprite's own nine-slice path (built for
/// UI chrome, where a stretch is the correct behaviour -- a parchment panel has no "face" to
/// distort), this is for a native sprite whose own aspect must survive into a differently-shaped
/// destination rect (an NPC portrait, portrait-oriented, into a landscape photo slot). Pure
/// function of two already-decoded images' dimensions; does not touch mtas_art.*/Art (data-lane
/// owned) -- composes on top of its already-public Art::ResolveSprite (native-resolution decode).
ce_unity::Image ContainFit(const ce_unity::Image& src, std::uint32_t out_w, std::uint32_t out_h) {
    ce_unity::Image out;
    out.width = out_w;
    out.height = out_h;
    out.rgba.assign(static_cast<std::size_t>(out_w) * out_h * 4, 0);
    if (src.width == 0 || src.height == 0 || src.rgba.empty() || out_w == 0 || out_h == 0)
        return out;
    const double scale = std::min(static_cast<double>(out_w) / static_cast<double>(src.width),
                                  static_cast<double>(out_h) / static_cast<double>(src.height));
    const auto dw = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(static_cast<double>(src.width) * scale));
    const auto dh = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(static_cast<double>(src.height) * scale));
    const std::uint32_t ox = dw < out_w ? (out_w - dw) / 2 : 0; // centred horizontally
    const std::uint32_t oy = dh < out_h ? (out_h - dh) / 2 : 0; // centred vertically
    for (std::uint32_t y = 0; y < dh; ++y) {
        const std::uint32_t sy = std::min(src.height - 1, y * src.height / dh);
        const std::uint8_t* srow = &src.rgba[static_cast<std::size_t>(sy) * src.width * 4];
        std::uint8_t* drow = &out.rgba[(static_cast<std::size_t>(oy + y) * out_w + ox) * 4];
        for (std::uint32_t x = 0; x < dw; ++x) {
            const std::uint32_t sx = std::min(src.width - 1, x * src.width / dw);
            std::memcpy(drow + static_cast<std::size_t>(x) * 4, srow + static_cast<std::size_t>(sx) * 4,
                       4);
        }
    }
    return out;
}


ce_unity::Image VisibleFit(const ce_unity::Image& src, std::uint32_t w, std::uint32_t h) {
    std::uint32_t x0 = src.width, y0 = src.height, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < src.height; ++y)
        for (std::uint32_t x = 0; x < src.width; ++x)
            if (src.rgba[(static_cast<std::size_t>(y) * src.width + x) * 4 + 3] > 40) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
            }
    if (x1 <= x0 || y1 <= y0) return ContainFit(src, w, h);
    ce_unity::Image crop{x1-x0, y1-y0, std::vector<std::uint8_t>(static_cast<std::size_t>(x1-x0) * (y1-y0) * 4)};
    for (std::uint32_t y = y0; y < y1; ++y)
        std::memcpy(crop.rgba.data() + static_cast<std::size_t>(y-y0) * crop.width * 4,
                    src.rgba.data() + (static_cast<std::size_t>(y) * src.width + x0) * 4,
                    static_cast<std::size_t>(crop.width) * 4);
    return ContainFit(crop, w, h);
}


EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (!p || !key || !sink)
            return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Module*>(p);
        if (!m.art)
            return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        std::optional<ce_unity::Image> img;
        // module:mtas:font
        // module:mtas:ui:<PinName>:<w>:<h>   (PinName is one of mtas::UiPins()'s/SpriteIndex()'s
        //                                     keys; nine-sliced/stretched to w,h -- UI chrome and
        //                                     item icons, which have no "face" to distort)
        // module:mtas:fit:<PinName>:<w>:<h>  (round-4: NPC portraits -- CONTAIN-fit, see
        //                                     ContainFit()'s own comment)
        if (k == "module:mtas:gradient:settings" || k == "module:mtas:gradient:fade") {
            const bool settings = k.ends_with("settings");
            ce_unity::Image gradient{1, 256, std::vector<std::uint8_t>(256 * 4)};
            for (int y = 0; y < 256; ++y) {
                gradient.rgba[y*4] = settings ? 29 + y*45/255 : 255;
                gradient.rgba[y*4+1] = settings ? 37 + y*42/255 : 247;
                gradient.rgba[y*4+2] = settings ? 63 + y*68/255 : 235;
                gradient.rgba[y*4+3] = settings ? 255 : y;
            }
            img = std::move(gradient);
        } else if (k.starts_with("module:mtas:fog:")) {
            // The fog-art mask (alpha = fog) or one tile's reveal mask, current keys only.
            std::lock_guard lock{m.live_fog_mutex};
            if (k == "module:mtas:fog:closed") {
                img = ce_unity::Image{1, 1, {255, 255, 255, 255}};
            } else {
                const auto* found = m.live_fog.Find(k);
                if (!found)
                    return EDEN_DSMOD_FALSE;
                img = *found;
            }
        } else if (constexpr std::string_view prefix = "module:mtas:label:"; k.starts_with(prefix)) {
            const auto hex = k.substr(prefix.size());
            if (hex.size() > 512 || hex.size() % 2) return EDEN_DSMOD_FALSE;
            std::string text;
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
                const int a = digit(hex[i]), b = digit(hex[i + 1]);
                if (a < 0 || b < 0) return EDEN_DSMOD_FALSE;
                text += static_cast<char>(a * 16 + b);
            }
            img = m.art->LabelImage(text, 140, 40, 18);
        } else if (constexpr std::string_view prefix = "module:mtas:badge:"; k.starts_with(prefix)) {
            const auto parsed = ParseNameWH(k.substr(prefix.size()));
            if (!parsed) return EDEN_DSMOD_FALSE;
            img = m.art->LoadMapBadge(std::get<0>(*parsed));
        } else if (k == "module:mtas:version") {
            img = m.art->VersionLabel();
        } else if (constexpr std::string_view prefix = "module:mtas:font/"; k.starts_with(prefix)) {
            const auto digits = k.substr(prefix.size());
            if (digits.empty() || digits.size() > 4 ||
                digits.find_first_not_of("0123456789") != std::string_view::npos)
                return EDEN_DSMOD_FALSE;
            img = m.art->FontAtlasImage(static_cast<std::uint32_t>(std::stoul(std::string{digits})));
        } else if (constexpr std::string_view prefix = "module:mtas:visible:"; k.starts_with(prefix)) {
            const auto parsed = ParseNameWH(k.substr(prefix.size()));
            if (!parsed) return EDEN_DSMOD_FALSE;
            const auto& [name, w, h] = *parsed;
            const auto native = m.art->ResolveSprite(name);
            if (native) img = VisibleFit(*native, w, h);
        } else if (k == "module:mtas:rank_star:filled" || k == "module:mtas:rank_star:empty") {
            const auto native = m.art->ResolveSprite("star-1");
            if (native) img = VisibleFit(*native, 40, 40);
            if (img && k == "module:mtas:rank_star:empty") {
                for (std::size_t i = 0; i < img->rgba.size(); i += 4) {
                    const auto grey = static_cast<std::uint8_t>((299 * img->rgba[i] + 587 * img->rgba[i+1] + 114 * img->rgba[i+2]) / 1000);
                    img->rgba[i] = img->rgba[i+1] = img->rgba[i+2] = grey;
                    img->rgba[i+3] = static_cast<std::uint8_t>(img->rgba[i+3] * 35 / 100);
                }
            }
        } else if (constexpr std::string_view prefix = "module:mtas:muted:"; k.starts_with(prefix)) {
            const auto parsed = ParseNameWH(k.substr(prefix.size()));
            if (!parsed) return EDEN_DSMOD_FALSE;
            const auto& [name, w, h] = *parsed;
            img = m.art->LoadSprite(name, w, h);
            if (img) for (std::size_t i = 0; i < img->rgba.size(); i += 4) {
                const auto grey = static_cast<std::uint8_t>((54 * img->rgba[i] + 183 * img->rgba[i+1] + 19 * img->rgba[i+2]) / 256);
                img->rgba[i] = img->rgba[i+1] = img->rgba[i+2] = grey;
            }
        } else if (constexpr std::string_view prefix = "module:mtas:ui:"; k.starts_with(prefix)) {
            const auto parsed = ParseNameWH(k.substr(prefix.size()));
            if (!parsed)
                return EDEN_DSMOD_FALSE;
            const auto& [name, w, h] = *parsed;
            img = m.art->LoadSprite(name, w, h);
        } else if (constexpr std::string_view prefix = "module:mtas:map:"; k.starts_with(prefix)) {
            // module:mtas:map:<col>:<row>:<px>  one MapGrid tile of the native world map
            int col{}, row{};
            std::uint32_t px{};
            if (!ParseMapTileKey(k.substr(prefix.size()), col, row, px))
                return EDEN_DSMOD_FALSE;
            img = m.art->LoadMapTile("MapWorld", static_cast<std::uint32_t>(col),
                                     static_cast<std::uint32_t>(row), MapGrid, px);
        } else if (constexpr std::string_view prefix = "module:mtas:fit:"; k.starts_with(prefix)) {
            const auto parsed = ParseNameWH(k.substr(prefix.size()));
            if (!parsed)
                return EDEN_DSMOD_FALSE;
            const auto& [name, w, h] = *parsed;
            auto native = m.art->ResolveSprite(name);
            if (!native && name.ends_with("_Picture")) native = m.art->ResolveSprite(name.substr(0, name.size() - 8));
            if (!native)
                return EDEN_DSMOD_FALSE;
            img = ContainFit(*native, w, h);
        } else {
            return EDEN_DSMOD_FALSE;
        }
        if (!img || img->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, img->width, img->height, img->rgba.data(), img->rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t*, std::size_t, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        auto* m = static_cast<Module*>(p);
        if (!m || !m->art || !sink)
            return EDEN_DSMOD_FALSE;
        std::uint32_t first{}, line{};
        std::vector<EdenDsmodFontGlyph> glyphs;
        if (!m->art->FontGlyphs(first, glyphs, line) || glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, line, first, glyphs.data(), static_cast<std::uint32_t>(glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "My Time at Sandrock DSMod",
                                   // Hidden ticks release the native HUD lease immediately.
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};

const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};

// Read-only save access: the Map page's fog baseline (MapModule's own saved mask).
const EdenDsmodModuleSaveExtensions SaveExtensions{EDEN_DSMOD_SAVE_EXT_VERSION,
                                                  sizeof(EdenDsmodModuleSaveExtensions),
                                                  EDEN_DSMOD_SAVE_EXT_HASH, &ConfigureSaveCallback};

void ConfigureTextureCallback(void* p, const EdenDsmodHostTextureApi* api) {
    if (p) static_cast<Module*>(p)->live_fog.Configure(api);
}
const EdenDsmodModuleTextureExtensions TextureExtensions{
    EDEN_DSMOD_TEXTURE_EXT_VERSION, sizeof(EdenDsmodModuleTextureExtensions),
    EDEN_DSMOD_TEXTURE_EXT_HASH, &ConfigureTextureCallback};

const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};

} // namespace
} // namespace mtas

DSMOD_SDK_EXPORT_MODULE(mtas::ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(mtas::ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(mtas::FontExtensions)
DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_save_extensions, EdenDsmodModuleSaveExtensions,
                        EDEN_DSMOD_SAVE_EXT_VERSION, EDEN_DSMOD_SAVE_EXT_HASH,
                        mtas::SaveExtensions)

DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_texture_extensions, EdenDsmodModuleTextureExtensions,
                        EDEN_DSMOD_TEXTURE_EXT_VERSION, EDEN_DSMOD_TEXTURE_EXT_HASH,
                        mtas::TextureExtensions)
