// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_problems.h"

#include <algorithm>
#include <array>

namespace cotl {

namespace {

struct Found {
    ProblemKind kind;
    std::string reason;
};

bool IsCursedState(int32_t t) {
    return t == kThoughtZombie || (t >= kThoughtFreezing && t <= kThoughtFreezing + 3);
}

// All problems of a follower, sorted by urgency
std::vector<Found> FollowerProblems(const Follower& f) {
    std::vector<Found> out;
    const int32_t st = f.cursed_state;
    if (st == kThoughtStarving || f.starvation > 0) {
        out.push_back({ProblemKind::Starving, "Starving"});
    }
    if (st == kThoughtIll) {
        out.push_back({ProblemKind::Ill, "Ill"});
    }
    if (st == kThoughtInjured) {
        out.push_back({ProblemKind::Injured, "Injured"});
    }
    if (f.satiation < kHungerThreshold && st != kThoughtStarving && f.starvation <= 0) {
        out.push_back({ProblemKind::Hungry, "Hungry"});
    }
    if (IsCursedState(st)) {
        out.push_back({ProblemKind::Cursed, Terms::EnumName("Thought", st)});
    }
    if (st == kThoughtDissenter) {
        out.push_back({ProblemKind::Dissent, "Dissenting"});
    }
    if (f.exhaustion > 0) { // state change in FollowerBrainStats.set_Exhaustion at > 0
        out.push_back({ProblemKind::Exhausted, "Exhausted"});
    }
    if (st == kThoughtOldAge || f.old_age) {
        out.push_back({ProblemKind::Old, "Old age"});
    }
    // Sort by urgency (ProblemKind), independent of the check order above
    std::stable_sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        return static_cast<int32_t>(a.kind) < static_cast<int32_t>(b.kind);
    });
    return out;
}

} // namespace

std::vector<ProblemRow> DeriveProblems(const GameSnapshot& s, const Terms&, int32_t filter) {
    std::vector<ProblemRow> rows;
    for (const auto& f : s.followers) {
        const auto found = FollowerProblems(f);
        if (found.empty()) {
            continue;
        }
        // Kind shown: the most urgent, or the filtered one when a filter is active
        auto shown = found.begin();
        if (filter >= 0) {
            shown = std::find_if(found.begin(), found.end(), [&](const Found& x) {
                return static_cast<int32_t>(x.kind) == filter;
            });
            if (shown == found.end()) {
                continue;
            }
        }
        ProblemRow row;
        row.follower_id = f.id;
        row.name = f.name;
        row.level = f.level;
        row.kind = shown->kind;
        row.reason = shown->reason;
        if (found.size() > 1) {
            // No parentheses: from the early built-in font, which had none
            row.reason += ", +" + std::to_string(found.size() - 1) + " more";
        }
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const ProblemRow& a, const ProblemRow& b) {
        return static_cast<int32_t>(a.kind) < static_cast<int32_t>(b.kind);
    });
    return rows;
}

int32_t ProblemSelection::Update(int64_t flag, const std::vector<ProblemRow>& rows) {
    if (flag != flag_) {
        flag_ = flag;
        follower_id_ = flag >= 0 && static_cast<size_t>(flag) < rows.size()
                           ? rows[static_cast<size_t>(flag)].follower_id
                           : -1;
    }
    return follower_id_;
}

void PublishSelectedProblems(Publisher& p, const GameSnapshot& s, int32_t follower_id) {
    const auto f = std::find_if(s.followers.begin(), s.followers.end(),
                                [&](const Follower& x) { return x.id == follower_id; });
    if (follower_id < 0 || f == s.followers.end()) {
        p.I64("prob.sel_count", 0);
        return;
    }
    const auto found = FollowerProblems(*f);
    p.Text("prob.sel_name", f->name);
    p.I64("prob.sel_level", f->level);
    p.I64("prob.sel_count", static_cast<int64_t>(found.size()));
    for (size_t i = 0; i < found.size(); ++i) {
        const std::string n = std::to_string(i);
        p.I64("prob.sel_kind" + n, static_cast<int64_t>(found[i].kind));
        p.Text("prob.sel_reason" + n, found[i].reason);
    }
}

void PublishProblems(Publisher& p, const GameSnapshot& s, const std::vector<ProblemRow>& rows) {
    // Count per kind across all problems, not just the most urgent per follower
    std::array<int64_t, kProblemKinds> counts{};
    int64_t total = 0; // affected followers, independent of the filter (number in the tab bar)
    for (const auto& f : s.followers) {
        const auto found = FollowerProblems(f);
        total += found.empty() ? 0 : 1;
        for (const auto& x : found) {
            ++counts[static_cast<size_t>(x.kind)];
        }
    }
    p.I64("prob.total", total);
    for (int k = 0; k < kProblemKinds; ++k) {
        p.I64("prob.n" + std::to_string(k), counts[static_cast<size_t>(k)]);
    }
    p.I64("prob.count", static_cast<int64_t>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::string n = std::to_string(i);
        p.Text("prob.name" + n, rows[i].name);
        p.I64("prob.level" + n, rows[i].level);
        p.I64("prob.kind" + n, static_cast<int64_t>(rows[i].kind));
        p.Text("prob.reason" + n, rows[i].reason);
    }
}

} // namespace cotl
