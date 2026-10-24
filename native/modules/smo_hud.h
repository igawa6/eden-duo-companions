// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "0100000000010000.h"
#include "core/mods/dsmod_module_extensions.h"
#include <vector>

namespace SmoHud {
// Per-instance native root-pane visibility only. No shared code/vtable patch and
// no actor alive/nerve edits. The original visibility bit is owned only after an
// expect-checked successful write; unrelated pane flags are preserved on restore.
class Controller {
public:
    void SetWriteApi(const EdenDsmodHostWriteApi& api);
    void Update(const EdenDsmodHostApi& host, const SmoReader::Snapshot& scene, bool hide);
    void Restore(const EdenDsmodHostApi& host);
    unsigned HiddenCount() const { return static_cast<unsigned>(owned_.size()); }
    // Per-update bounded diagnostics. Early exits 1..9; deepest actor path 10..17.
    unsigned GetterStage() const { return stage_; }
    unsigned CandidateCount() const { return candidates_; }
    unsigned PaneCount() const { return panes_; }
    unsigned VisibleCount() const { return visible_; }
    unsigned WriteAttempts() const { return attempts_; }
    unsigned FailedWrites() const { return failures_; }
    std::uint64_t ProbePane() const { return probe_pane_; }
    unsigned ProbeFlags() const { return probe_flags_; }
private:
    struct Pane { std::uint64_t actor, vtable, keeper, layout, pane; };
    bool Valid(const EdenDsmodHostApi& host, const Pane& pane) const;
    bool Write(const EdenDsmodHostApi& host, const Pane& pane, bool visible) const;
    EdenDsmodHostWriteApi write_{};
    std::vector<Pane> owned_;
    unsigned stage_ = 0, candidates_ = 0, panes_ = 0, visible_ = 0;
    unsigned attempts_ = 0, failures_ = 0, probe_flags_ = 0;
    std::uint64_t probe_pane_ = 0;
};
} // namespace SmoHud
