// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion: everything the live lane publishes (CONTRACT.md "Published names" plus the
// names the pages need, research/acnh/impl/ui/REPORT.md). Values come from the live snapshot
// (acnh_live.h) joined with the game's tables and text (acnh_catalog.h, the GAME language).
//
// Lazy: the open page (module action ui_open 1..6, echoed as ui.page) decides what is read and
// composed; the status, clock and language values are always published. The host clears the
// snapshot every sample, so composed values are cached and re-emitted, and only rebuilt when
// their inputs change. Module actions: ui_open, res_sel, bag_sel, crit_kind, crit_kind_step,
// crit_filter, crit_sel, diy_cat, diy_cat_step, diy_filter, diy_sel, phone_open.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "acnh_catalog.h"
#include "acnh_live.h"
#include "acnh_live_extra.h"
#include "acnh_types.h"

namespace acnh {

/// Width the runtime measures for `text` at text_scale 10 with the package font ("ui:40":
/// Seurat-B + ParkExt from the player's romfs), i.e. Canvas::MeasureText(text, 10).
class TextWidth {
public:
    /// `fonts` = the module's faces; null = a private Fonts on `romfs` (tests).
    TextWidth(Romfs* romfs, Fonts* fonts);
    ~TextWidth();
    int Measure(const std::string& utf8);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Publisher {
public:
    explicit Publisher(const Services& services);
    ~Publisher();

    /// Which parts the next sample has to read (the open page).
    live::Need WhatToRead() const;
    void Publish(const EdenDsmodHostApi& host, const live::LiveSnapshot& s, int64_t sample_us);
    bool Action(const char* action, int64_t argument);

    int Page() const {
        return page;
    }
    /// The Pockets page's selected slot, -1 = none (r9: the default).
    int BagSel() const {
        return bag_sel;
    }
    bool PhoneDriveBusy() const {
        return pdrv_target >= 0;
    }

private:
    struct Out {
        std::vector<std::pair<std::string, int64_t>> ints;
        std::vector<std::pair<std::string, std::string>> texts;
        uint64_t key = ~uint64_t{0}; ///< hash of the inputs it was built from
        void Clear() {
            ints.clear();
            texts.clear();
        }
        void I(std::string n, int64_t v) {
            ints.emplace_back(std::move(n), v);
        }
        void T(std::string n, std::string v) {
            texts.emplace_back(std::move(n), std::move(v));
        }
    };
    void Emit(const EdenDsmodHostApi& host, const Out& o);

    void BuildStatic(const live::LiveSnapshot& s); ///< txt.* in the game language
    void BuildClock(const live::LiveSnapshot& s);
    void BuildMap(const live::LiveSnapshot& s);
    /// The map pin (player.x / player.z): the position outdoors, the building's icon indoors.
    void PublishPin(const EdenDsmodHostApi& host, const live::LiveSnapshot& s);
    void BuildBag(const live::LiveSnapshot& s);
    void BuildCritters(const live::LiveSnapshot& s);
    void BuildDiy(const live::LiveSnapshot& s);
    void BuildToday(const live::LiveSnapshot& s);
    void BuildPhone(const live::LiveSnapshot& s);
    void DrivePhone(const live::LiveSnapshot& s, Out& o);
    /// acnh.diag.* (hidden overlay ui.diag, 5 taps on the phone clock) + one log line per change
    void BuildDiag(const EdenDsmodHostApi& host, const live::LiveSnapshot& s, int64_t sample_us);
    bool DiagAction(std::string_view action);
    std::vector<int> InstalledApps(const live::LiveSnapshot& s, bool& from_game);

    std::string Msg(std::string_view path, std::string_view label, std::string_view fallback = {});
    /// A message whose date tags (group 90: 34 year, 35 month = String/STR_Month, 36 day =
    /// String/STR_Day) are filled in, in the message's own order; "" when the label is missing.
    std::string DateMsg(std::string_view path, std::string_view label, int y, int m, int d);
    std::string Num(int64_t v) const; ///< thousands grouping
    /// Player event flag value by EventFlagsPlayerParam key (-1 = unknown).
    int PlayerFlag(const live::LiveSnapshot& s, std::string_view name);
    /// UniqueID of an EventFlags<table> key ("EventFlagsPlayerParam" / "EventFlagsLandParam"), -1.
    int FlagUid(std::string_view table, std::string_view name);
    /// SeasonCalendar column `name` (_N/_S by hemisphere) of the game day (hours 0-4 = the day
    /// before): the first row whose Month/Day is on or after that day (0x98ae20); -1 unknown.
    int SeasonColumn(const live::LiveSnapshot& s, const char* name);
    /// The game's weather type at the current hour (GetWeatherType 0x98d8a8), -1 unknown.
    int WeatherNow(const live::LiveSnapshot& s);
    /// Calendar-event day test of the event manager (CalendarEventParam), for IsSeasonal's
    /// SelectCalendarEvent rows: `label` on the game day (or within its ReadyDays before it).
    bool EventOn(const live::LiveSnapshot& s, std::string_view label, bool with_ready);
    /// The event manager's date test for one CalendarEventParam row on `day` (days since
    /// 1970-01-01; 0x775530 without the label dispatch): year rule, date columns, Term span.
    bool RowOn(const Bcsv& t, size_t row, int64_t day, int hemi) const;
    /// K.K. Slider's concert on the game day (0x27d4550, round 7): 0 none, 1 NormalLive,
    /// 2 BirthdayLive, 3 FirstLive, -1 unknown (inputs not read).
    int KkDay(const live::LiveSnapshot& s, const live::ExtraSnapshot& x);
    /// The DIY app's Seasonal Recipes rule for one recipe (today, hemisphere).
    bool SeasonalRecipe(const live::LiveSnapshot& s, const Catalog::Recipe& r);
    /// `text` as the game writes it into message `path`/`label`: when the label starts with the
    /// capitalise tag (group 50 type 3) the first character goes through the game's case table
    /// (snapshot case_table; Dutch "ij" -> "IJ"), else unchanged.
    std::string GameCase(const live::LiveSnapshot& s, std::string_view path, std::string_view label,
                         std::string text);

    const Services services;
    TextWidth widths;
    int lang = -1; ///< Lang of the texts built so far

    // UI state (module actions)
    int page = 1; ///< 1 map .. 6 phone
    // r9 pop, picture prewarm: from the first sample with the island read (acnh.ready) every
    // page's values are published while ui.page stays 0, so the waiting page (whose hidden copies
    // of the six pages' pictures are gated on ui.prewarm) makes the runtime load each page's
    // pictures before the first page shows; it ends once no load_image has run for PrewarmIdleMs
    // (the host's asset queue is empty), after PrewarmMinMs, at the latest PrewarmMaxMs.
    static constexpr uint64_t PrewarmMinMs = 800, PrewarmIdleMs = 400, PrewarmMaxMs = 6000;
    bool prewarm_done = false;
    uint64_t prewarm_start_ms = 0;
    bool Prewarming() const {
        return services.image_activity_ms && !prewarm_done;
    }
    int bag_sel = -1, crit_kind = 0, crit_filter = 0, crit_sel = 0; ///< bag: none selected (r9)
    int diy_cat = 0, diy_filter = 0, diy_sel = 0;
    int app_sel = -1;
    bool phone_available = false;
    // NookPhone pager (round 7): the page shown (0 = apps 1-9, 1 = apps 10-18) and when it last
    // changed. The page's slide is a 250 ms anim group, and runtime 15 hit-tests the incoming
    // page at its final rects from the first frame (no "group moving" value, anim groups do not
    // hold input like page transitions do), so the module times the slide: ui.phone.pg drives the
    // anim, ui.phone.slide is 1 while it runs, and phone_open is refused inside that window.
    int phone_pg = 0;
    uint64_t phone_pg_ms = 0;
    uint32_t crit_list = 0, diy_list = 0;
    /// crit.sel.par: flips whenever the Critterpedia selection (or its list) changes, so the page
    /// re-fades the caption plate that follows the selected cell (r9 polish)
    int crit_par = 0, crit_par_sel = -2;
    uint32_t crit_par_list = 0;

    // NookPhone drive (P5R pattern): one press per settled state toward app id `pdrv_target`
    int pdrv_target = -1; ///< app id (0..17), -1 = idle
    bool pdrv_pressed_a = false;
    int pdrv_settle = 0, pdrv_waiting = 0, pdrv_steps = 0;
    uint64_t pdrv_start_ms = 0;
    uint64_t pdrv_home_ms =
        0; ///< first sample the home grid was up during this drive (0 = not yet)
    uint64_t pdrv_press_ms = 0; ///< time of the last press
    bool pdrv_last_dpad = false;
    std::string pdrv_seen;
    std::vector<int> apps; ///< published app ids (phone order)
    // diagnostics overlay (acnh_publish_diag.cpp)
    bool diag_on = false;
    int diag_taps = 0;
    uint64_t diag_tap_ms = 0, diag_log_key = 0, diag_log_ms = 0;

    Out o_static, o_clock, o_map, o_bag, o_crit, o_diy, o_today, o_phone, o_status;
    std::vector<int> crit_rows; ///< catalog critter indices shown (kind crit_kind, filtered)
    std::array<std::vector<uint16_t>, 3>
        book_items; ///< Critterpedia lists (PictureBook BYML item ids)
    bool book_tried = false;
    std::array<std::vector<int>, 3>
        book_art;              ///< critters with book art (the list without the BYML)
    std::vector<int> diy_rows; ///< recipe indices shown
    std::vector<int> diy_cats; ///< RECIPE_CATS indices present
    uint32_t island_rev_sent = 0;
    /// r9 indoor: StructureInfoParam UniqueID -> its map icon centre in world units (x, z), from
    /// the Map page's icon list (BuildMap)
    std::unordered_map<int, std::pair<float, float>> pin_anchor;
    std::unordered_map<std::string, int> flag_uid; ///< "<table>/<key>" -> UniqueID
    live::ExtraReader extra; ///< data lane: Critterpedia mirror, NM+ x5, special visitors
    live::ExtraSnapshot xs;
};

} // namespace acnh
