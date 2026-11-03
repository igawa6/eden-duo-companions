// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Problems and quests per follower, derived from the snapshot. Thresholds and states from the
// game code, see tools/cotl/README.md (problems).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cotl_publish.h"
#include "cotl_snapshot.h"
#include "cotl_terms.h"

namespace cotl {

// Order = urgency (lower = more urgent)
enum class ProblemKind : int32_t {
    Starving = 0,
    Ill = 1,
    Injured = 2,
    Hungry = 3,
    Cursed = 4, // freezing, overheating, soaked, burning, zombie
    Dissent = 5,
    Exhausted = 6,
    Old = 7,
};
inline constexpr int kProblemKinds = 8;

// FollowerInfo constants (dump.cs) and Thought values of FollowerInfo.CursedState
inline constexpr float kHungerThreshold = 30;   // FollowerInfo.HUNGER_THRESHOLD
inline constexpr int32_t kThoughtDissenter = 66;
inline constexpr int32_t kThoughtIll = 68;
inline constexpr int32_t kThoughtStarving = 131; // BecomeStarving
inline constexpr int32_t kThoughtOldAge = 146;
inline constexpr int32_t kThoughtZombie = 181;
inline constexpr int32_t kThoughtInjured = 310;
inline constexpr int32_t kThoughtFreezing = 480;  // up to 483: Overheating, Soaking, Aflame
inline constexpr float kGameMinutesPerDay = 1200; // TimeManager.GAME_MINUTES_PER_DAY

struct ProblemRow {
    int32_t follower_id = 0;
    std::string name;
    int32_t level = 0;
    ProblemKind kind = ProblemKind::Old;
    std::string reason;
};

// One row per follower with their most urgent problem; further problems as "+N more" in the
// reason. Sorted by urgency. Quests do not count (separate Quests tab).
// filter >= 0: only followers with a problem of this kind (ProblemKind), the row shows this kind.
std::vector<ProblemRow> DeriveProblems(const GameSnapshot& s, const Terms& terms,
                                       int32_t filter = -1);

// prob.count, prob.n<kind> (count per kind, several per follower possible), prob.name{i},
// prob.level{i}, prob.kind{i}, prob.reason{i}
void PublishProblems(Publisher& p, const GameSnapshot& s, const std::vector<ProblemRow>& rows);

// Remembers the follower when a row is tapped: rows can re-sort while
// the window is open. flag = row index (@flag:prob.open), -1 = closed.
class ProblemSelection {
public:
    // Follower of the open window, -1 if none is open or the row does not exist
    int32_t Update(int64_t flag, const std::vector<ProblemRow>& rows);

private:
    int64_t flag_ = -1;
    int32_t follower_id_ = -1;
};

// All problems of the follower, by urgency: prob.sel_name, prob.sel_level, prob.sel_count,
// prob.sel_kind{i}, prob.sel_reason{i}. Without a matching follower only prob.sel_count = 0.
void PublishSelectedProblems(Publisher& p, const GameSnapshot& s, int32_t follower_id);

} // namespace cotl
