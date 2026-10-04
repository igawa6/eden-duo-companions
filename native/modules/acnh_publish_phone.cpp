// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion publishing: the NookPhone page (acnh_publish.h): the installed apps and the
// button drive that opens one in the game.

#include "acnh_publish.h"
#include "acnh_publish_detail.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "acnh_bcsv.h"
#include "acnh_catalog.h"
#include "acnh_lang.h"
#include "acnh_lang_grammar.h"
#include "acnh_map.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"

namespace acnh {
using namespace pub;

// ---- phone page ------------------------------------------------------------------------------
// The installed apps, in the game's own home-grid order. While the home grid exists the game's
// list is used as is (seq +0x230); otherwise the builder's rule (0x2e5d6dc / IsAppInstalled
// 0x2e5a0e0) is replayed on the live EventFlags: an app with an unlock flag needs that flag; Nook
// Miles (6) only while Nook Miles+ (7) is locked; DIY Recipes (3) only while DIY Recipes+ (4) is
// locked and (code: a recipe-count over a code mask, LEAD here) the player knows a recipe.
std::vector<int> Publisher::InstalledApps(const live::LiveSnapshot& s, bool& from_game) {
    from_game = false;
    std::vector<int> out;
    if (s.phone_ui.ok && s.phone_ui.count > 0) {
        from_game = true;
        return s.phone_ui.ids;
    }
    if (s.app_defs.size() != 18 || !s.flags_ok || !services.catalog)
        return out;
    if (!services.catalog->Table("EventFlagsPlayerParam"))
        return out;
    // the flag's UniqueID from the cached lookup (was a scan of the whole table per app per sample)
    const auto flag = [&](const std::string& name) -> int {
        const int uid = FlagUid("EventFlagsPlayerParam", name);
        return uid >= 0 && uid <= 0x7ff ? s.event_flags[uid] : 0;
    };
    const auto def = [&](int id) -> const live::LiveSnapshot::AppDef* {
        for (const auto& d : s.app_defs)
            if (d.id == id)
                return &d;
        return nullptr;
    };
    const auto installed = [&](int id) {
        const auto* d = def(id);
        if (!d)
            return false;
        if (d->flag.empty())
            return true;
        if (!flag(d->flag))
            return false;
        if (id == 6 || id == 3) {
            const auto* n = def(id == 6 ? 7 : 4);
            if (!n || n->flag.empty() || flag(n->flag))
                return false;
            if (id == 3)
                return std::any_of(s.recipe_collect.begin(), s.recipe_collect.end(),
                                   [](uint8_t b) { return b != 0; });
        }
        return true;
    };
    for (const auto& d : s.app_defs)
        if (installed(d.id))
            out.push_back(d.id);
    return out;
}

void Publisher::BuildPhone(const live::LiveSnapshot& s) {
    bool from_game = false;
    apps = InstalledApps(s, from_game);
    const int sel = s.phone_open && s.phone_ui.ok ? s.phone_ui.cursor : -1;
    uint64_t key = Mix(0x9605e, static_cast<uint64_t>(lang + 1));
    for (const int id : apps)
        key = Mix(key, static_cast<uint64_t>(id));
    key = Mix(key, (uint64_t(sel + 1) << 8) | from_game);
    if (o_phone.key == key)
        return;
    o_phone.Clear();
    o_phone.key = key;
    o_phone.I("app.n", static_cast<int64_t>(apps.size()));
    for (size_t i = 0; i < apps.size(); ++i) {
        const live::LiveSnapshot::AppDef* d = nullptr;
        for (const auto& x : s.app_defs)
            if (x.id == apps[i])
                d = &x;
        if (!d)
            continue;
        // the tile picture (manifest src_bind app.{i+0}.icon / app.{i+9}.icon). The r8 prune
        // dropped it as unread although both pages bind it: no icons, so no visible tap targets
        // on the NookPhone page (owner, Thor rc9 2026-10-03; Linux the same)
        const std::string p = "app." + std::to_string(i) + ".";
        o_phone.T(p + "icon", std::string{Key} + "icon/app/" + std::to_string(d->frame) + "?s=132");
        o_phone.T(p + "name", Msg("LayoutMsg/MenuDevice", d->label));
    }
    app_sel = sel >= 0 && sel < static_cast<int>(apps.size()) ? sel : -1;
    o_phone.I("app.sel", app_sel);
}

// NookPhone drive: one press per settled state (the observed phone state, home-grid cursor/page
// and the player's free flag unchanged for a few samples), re-planned from the live cursor each
// time, so a dropped press is simply retried. Opening the phone (ZL) only while the player is free.
void Publisher::DrivePhone(const live::LiveSnapshot& s, Out& o) {
    // One press per settled state. Budgets (fix lane, 2026-10-02): the first open of the phone is
    // slow (ZL -> cChangeSmartPhone -> the seq's cCreateSeq/... -> cExecDevice with the grid built,
    // well over 10 s headless), so the cursor-move budget (20 s) starts only when the home grid is
    // first up (cExecDevice + the grid's list, phone_ui.ok), with an overall 60 s guard. The grid
    // ignores the D-pad during its opening animation, so a press that did not move the cursor is
    // re-sent after 700 ms of no change (a late-registered press only costs a re-plan: the target
    // is recomputed from the live cursor every step); A / B / ZL are re-sent after 1.5 s.
    constexpr int Settle = 3;
    constexpr uint64_t TotalMs = 60000, GridMs = 20000, HomeQuietMs = 700, DpadRetryMs = 700,
                       RetryMs = 1500;
    std::string press;
    if (!phone_available)
        pdrv_target = -1;
    if (pdrv_target >= 0) {
        const uint64_t now = live::NowMs();
        const auto finish = [&] { pdrv_target = -1; };
        const std::string seen = s.phone.name + "|" + std::to_string(s.phone_ui.ok) + "|" +
                                 std::to_string(s.phone_ui.cursor) + "|" +
                                 std::to_string(s.phone_ui.page) + "|" + std::to_string(s.free) +
                                 "|" + s.camera.name;
        if (seen != pdrv_seen) {
            pdrv_seen = seen;
            pdrv_settle = 0;
            pdrv_waiting = 0;
        }
        const bool home = s.phone_ok && s.phone.name == "cExecDevice" && s.phone_ui.ok;
        if (home && !pdrv_home_ms)
            pdrv_home_ms = now;
        const bool in_flight =
            pdrv_waiting && now - pdrv_press_ms < (pdrv_last_dpad ? DpadRetryMs : RetryMs);
        if (now - pdrv_start_ms > TotalMs || (pdrv_home_ms && now - pdrv_home_ms > GridMs) ||
            pdrv_steps > 60) {
            finish(); // timeout
        } else if (pdrv_pressed_a && !home && s.phone_open) {
            finish(); // done: the app left the home grid
        } else if (pdrv_settle < Settle) {
            ++pdrv_settle;
        } else if (in_flight) {
            // a press is in flight
        } else if (home && now - pdrv_home_ms < HomeQuietMs) {
            // the grid just came up (opening animation): no D-pad yet
        } else if (!s.phone_ok || !s.scene_ok) {
            finish(); // no scene
        } else if (!s.phone_open) {
            if (pdrv_pressed_a)
                finish(); // phone closed
            else if (s.camera_ok && s.camera.name == "cTakePhoto")
                press = "b"; // Camera closes the phone sequence: return to free play first.
            else if (s.free)
                press = "zl";
            else if (pdrv_steps == 0)
                finish(); // player busy: never start from a busy player
            // else: the phone is opening (player in cChangeSmartPhone): wait
        } else if (!home) {
            press = "b"; // inside another app: back to the home grid
        } else {
            const auto& ids = s.phone_ui.ids;
            const auto it = std::find(ids.begin(), ids.end(), pdrv_target);
            if (it == ids.end()) {
                // the grid is (re)built when the phone opens: wait for the target (timeout covers
                // an app that is really not installed)
            } else {
                const int t = static_cast<int>(it - ids.begin()), c = s.phone_ui.cursor;
                const int tp = t / 9, cp = c / 9, tr = t % 9 / 3, cr = c % 9 / 3, tc = t % 3,
                          cc = c % 3;
                if (c == t)
                    press = "a", pdrv_pressed_a = true;
                else if (tp != cp)
                    press = tp > cp ? "right"
                                    : "left"; // right of column 3 / left of column 1 turns the page
                else if (tr != cr)
                    press = tr > cr ? "down" : "up";
                else
                    press = tc > cc ? "right" : "left";
            }
        }
        if (!press.empty()) {
            ++pdrv_steps;
            pdrv_waiting = 1;
            pdrv_press_ms = now;
            pdrv_last_dpad =
                press == "up" || press == "down" || press == "left" || press == "right";
        }
    }
    o.I("pdrv.pending", press.empty() ? 0 : 1);
    o.I("pdrv.idle", pdrv_target < 0 ? 1 : 0);
    for (const char* b : {"a", "b", "x", "zl", "up", "down", "left", "right", "l", "r"})
        o.I(std::string{"pdrv."} + b, press == b ? 1 : 0);
}

} // namespace acnh
