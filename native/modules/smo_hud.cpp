// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_hud.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace SmoHud {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
template<class T> bool Read(const EdenDsmodHostApi& host, u64 at, T& out) {
    return at && at <= UINT64_MAX - sizeof out && host.is_mapped && host.read_memory &&
           host.is_mapped(host.userdata, at, sizeof out) &&
           host.read_memory(host.userdata, at, &out, sizeof out);
}
bool Vtable(const EdenDsmodHostApi& h, u64 at, u64 offset) {
    u64 vt = 0;
    const auto delta = h.get_i64 ? h.get_i64(h.userdata, "__relocation_delta", 0) : 0;
    return Read(h, at, vt) && (vt == h.main_base + offset ||
        vt == h.main_base + offset + static_cast<u64>(delta));
}
}
void Controller::SetWriteApi(const EdenDsmodHostWriteApi& api) {
    if (api.version == EDEN_DSMOD_WRITE_EXT_VERSION &&
        api.struct_size == sizeof api && api.abi_hash == EDEN_DSMOD_WRITE_EXT_HASH && api.write_batch)
        write_ = api;
}
bool Controller::Valid(const EdenDsmodHostApi& h, const Pane& p) const {
    u64 actor_vt = 0, keeper = 0, layout = 0, pane = 0;
    return Read(h, p.actor, actor_vt) && actor_vt == p.vtable &&
           Read(h, p.actor + 0xE0, keeper) && keeper == p.keeper &&
           Read(h, keeper + 0x10, layout) && layout == p.layout &&
           Read(h, layout + 0x18, pane) && pane == p.pane &&
           // eui::RootPane derives directly from nn::ui2d::Pane. Its native
           // ctor A26630 calls base ctor908970 before installing this vtable.
           (Vtable(h, pane, 0x132B248) || Vtable(h, pane, 0x135EDC8));
}
bool Controller::Write(const EdenDsmodHostApi& h, const Pane& p, bool visible) const {
    u8 before = 0;
    if (!write_.write_batch || !Valid(h, p) || !Read(h, p.pane + 0x58, before)) return false;
    const u8 after = visible ? static_cast<u8>(before | 1) : static_cast<u8>(before & ~1);
    if (before == after) return true;
    const EdenDsmodWriteOp op{p.pane + 0x58, 1, 0, &before, &after};
    return write_.write_batch(write_.userdata, &op, 1) != 0;
}
void Controller::Restore(const EdenDsmodHostApi& h) {
    // Restore only the bit this module cleared, while every ownership link still
    // points to the same objects. A recycled actor/pane is never touched.
    std::erase_if(owned_, [&](const Pane& p) { return !Valid(h, p) || Write(h, p, true); });
}
void Controller::Update(const EdenDsmodHostApi& h, const SmoReader::Snapshot& s, bool hide) {
    stage_ = candidates_ = panes_ = visible_ = attempts_ = failures_ = probe_flags_ = 0;
    probe_pane_ = 0;
    const bool aux = h.get_i64 && h.get_i64(h.userdata, "__aux_present", 0) != 0;
    if (!hide) stage_ = 1;
    else if (!aux) stage_ = 2;
    else if (!s.chain_ready) stage_ = 3;
    else if (!s.scene_mode_valid) stage_ = 4;
    else if (s.scene_mode != SmoReader::SceneMode::Playing) stage_ = 5;
    if (stage_) { Restore(h); return; }
    if (!write_.write_batch) { stage_ = 6; return; }
    if (!Vtable(h, s.stage_scene, SmoReader::kStageSceneVtableOffset)) { stage_ = 7; return; }
    std::erase_if(owned_, [&](const Pane& p) { return !Valid(h, p); });
    u64 holder = 0;
    if (!Read(h, s.stage_scene + 0x308, holder)) { stage_ = 8; return; }
    if (!Vtable(h, holder, 0x1212CA8)) { stage_ = 9; return; }
    stage_ = 10;
    const auto reached = [&](unsigned stage) { stage_ = std::max(stage_, stage); };
    const auto collect = [&](u64 actor, u64 expected) {
        if (!Vtable(h, actor, expected)) return;
        ++candidates_; reached(11);
        Pane p{actor, 0, 0, 0, 0};
        if (!Read(h, actor, p.vtable) || !Read(h, actor + 0xE0, p.keeper) ||
            !Read(h, p.keeper + 0x10, p.layout) || !Read(h, p.layout + 0x18, p.pane)) return;
        reached(12);
        if (!Valid(h, p)) return;
        ++panes_; reached(13);
        u8 flags = 0;
        if (!Read(h, p.pane + 0x58, flags)) return;
        reached(14);
        if (!probe_pane_) { probe_pane_ = p.pane; probe_flags_ = flags; }
        const bool owned = std::any_of(owned_.begin(), owned_.end(),
            [&](const Pane& old) { return old.pane == p.pane; });
        // Naturally invisible panes are never claimed or made visible later.
        if (!(flags & 1)) { reached(15); return; }
        ++visible_; ++attempts_;
        if (!Write(h, p, false)) { ++failures_; reached(16); return; }
        reached(17);
        if (!owned) owned_.push_back(p);
    };
    for (const auto [offset, vt] : std::array<std::pair<u64,u64>, 5>{{
            {0x18,0x12089E0}, {0x30,0x12089E0}, {0x28,0x12113A0},
            {0x50,0x120D370}, // MapMini: compass
            {0x68,0x1308A70}}}) { // SimpleLayoutAppearWaitEnd: PlayGuideMenu
        u64 actor = 0;
        if (Read(h, holder + offset, actor)) collect(actor, vt);
    }
    u64 mini = 0, mini_player = 0;
    if (Read(h, holder + 0x50, mini) && Vtable(h, mini, 0x120D370)) {
        if (Read(h, mini + 0x140, mini_player))
            collect(mini_player, 0x12ECB70); // separately drawn MapMiniPlayer compass arrow
        std::int32_t count = 0;
        u64 icons = 0;
        if (Read(h, mini + 0x148, count) && count >= 0 && count <= 10 &&
            Read(h, mini + 0x150, icons) && icons && icons <= UINT64_MAX - 10 * 8) {
            for (int i = 0; i < count; ++i) {
                u64 icon = 0;
                if (Read(h, icons + i * 8, icon)) collect(icon, 0x12ECB70); // MapMiniIcon
            }
        }
    }
    u64 life = 0;
    if (Read(h, holder + 0x20, life) && Vtable(h, life, 0x1209E50)) {
        for (u64 offset : {0x18,0x20,0x28}) {
            u64 actor = 0;
            if (Read(h, life + offset, actor)) collect(actor, 0x1209978);
        }
    }

}
} // namespace SmoHud
