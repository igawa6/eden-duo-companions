// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// All quests for the Quests tab: offered (followers with "!") and accepted, also story and
// NPC quests. Texts follow ObjectivesData.get_Text of each subclass, see tools/cotl/README.md.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cotl_publish.h"
#include "cotl_snapshot.h"
#include "cotl_terms.h"

namespace cotl {

// One step of a quest (one ObjectivesData)
struct QuestStep {
    std::string text;
    bool done = false;
    int32_t progress = -1; // -1 = unknown
    int32_t target = -1;
};

struct QuestView {
    std::string key;        // stays the same on re-sorting (selection in the window)
    std::string title;      // GroupId title
    std::string text;       // task, e.g. "Collect Berry"
    std::string giver;      // follower name or empty
    int32_t progress = -1;  // -1 = unknown
    int32_t target = -1;
    std::string due;        // "2 days left", "due today" or empty
    bool offered = false;   // not accepted yet
    int32_t giver_id = -1;  // follower ID of the offer (Walk over), otherwise -1
    std::vector<QuestStep> steps; // all sub-items, open ones first
};

// Replaces {0}, {1}, ... with args. If an argument is empty, a parenthesis "(...)" around its
// placeholder is removed with the leading space (e.g. unknown progress).
std::string FormatTerm(std::string_view fmt, const std::vector<std::string>& args);

std::vector<QuestView> BuildQuests(const GameSnapshot& s, const Terms& terms);

// quest.count, quest.title{i}, quest.text{i}, quest.giver{i}, quest.due{i}, quest.offered{i},
// quest.more{i}; for `selected`: quest.sel_title, quest.sel_text, quest.sel_giver,
// quest.sel_progress, quest.sel_due, quest.step_count, quest.step{i}, quest.step_done{i}
void PublishQuests(Publisher& p, const std::vector<QuestView>& quests, int64_t selected);

} // namespace cotl
