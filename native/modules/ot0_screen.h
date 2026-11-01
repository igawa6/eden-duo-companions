// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <optional>
#include "ot0_reader.h"
#include "ot0_results.h"
#include "ot0_pak.h"
#include "ot0_exploration.h"
#include "ot0_appearance.h"
#include "ot0_battle.h"
#include "ot0_equipment.h"
#include "ot0_minimap.h"
#include "ot0_gear.h"
#include "ot0_battle_selection.h"
#include "ot0_battle_stats.h"
#include "ot0_battle_status.h"
namespace ot0 {
struct FormationSlotRect {
    int x{}, y{}, w{312}, h{160};
    bool on{};
};
struct FormationLayout {
    bool solo{};
    std::array<FormationSlotRect, 8> slots{};
};
// Pure shared geometry for raster composition and the module's per-slot x/y bindings.
FormationLayout ComputeFormationLayout(const Snapshot& snapshot);

// Native alpha-holdout reveal stamps, applied before resizing. Without the
// native stamp contract, offline fixtures retain the conservative cell mask.
// Battle page geometry shared with the module's tap rectangles: focused foe panel and the
// compact rows of the other living foes (enemy indices in display order).
struct BattleLayout {
    static constexpr int FoeWidth=576;
    int focus{-1};
    std::array<int,6> others{-1,-1,-1,-1,-1,-1};
    int other_count{};
    static constexpr int RowTop(int count,int i) { return count>=3?132+(936/(count+1))*(i+1):1080-count*140+i*140-2; }
};
BattleLayout ComputeBattleLayout(const BattleSnapshot& battle,int focus);

// Shared map/party geometry: a nearly square native map pane beside four
// vertical front/back pairs, using the battle page's roster hierarchy.
inline constexpr u32 RingFrames=24,RingFrameMs=50,RingFrameSize=212;
// Party page sub-tab (0 Stats, 1 Equipment) and the selected member's equipped items.
struct PartyView {
    int tab{}; const Gear* gear{};
    // Items page (footer tab): native category tab 0..11 and the first listed row.
    bool items_page{}; int items_tab{}, items_first{}; const Inventory* inventory{};
    int* items_last_first{}; // out: the last page's first row for this tab
    bool quests_page{}; const QuestStates* quests{};
};
// Native Items menu tabs, in its column order.
inline constexpr int ItemTabs=12, ItemRows=12;
struct TownMapLayout {
    static constexpr int X=16,Y=94,W=760,H=828;
    static constexpr int InnerX=44,InnerY=122,InnerW=704,InnerH=772;
    static constexpr int PartyX=792,PartyW=432,PairTop=94,PairPitch=208,PairH=202;
};

Image MaskRevealedPaths(const Image& source,const ExplorationSnapshot& map,
                        const Image* reveal_mask=nullptr);

// Positions from the composed frame; native glyphs animate on the host GPU.
// spent: a BP pip the boosted command will use (grey, blinking) instead of a live red pip.
struct PulseGlyph { int x{}, y{}, size{}; bool player{}; std::array<u8,4> color{255,255,255,255}; bool spent{}; };
// Map picture geometry (runtime pan/zoom image): picture pixel (px,py) = native texture pixel
// (crop_x+px, crop_y+py); the picture has the map pane's aspect so the host's stretch keeps it
// square. Markers are positioned by the module every tick, not drawn into the page.
struct MapGeometry {
    bool valid{};
    uint32_t texture_w{},texture_h{},picture_w{},picture_h{};
    double crop_x{},crop_y{};
    uint64_t revision{};
    bool operator==(const MapGeometry&) const=default;
};
struct ScreenVisuals { std::vector<PulseGlyph> pulses; bool map_complete{}; MapGeometry map; };

struct BattleDetailView {
    const BattleSelection* selection{};
    // Host supplies only freshly ACK-matched parameter rows.
    const BattleStatsSnapshot* stats{};
    const BattleMetadata* metadata{};
    const BattleStatusSnapshot* statuses{};
    bool automatic{true};
};
// Field status values for one member, from the game's own Status computation (helper);
// order: Phys. Atk, Elem. Atk, Phys. Def, Elem. Def, Accuracy, Speed, Critical, Evasion.
struct MemberStats {
    bool valid{};
    uint32_t id{};
    std::array<int32_t,8> values{};
    bool operator==(const MemberStats&) const=default;
};
struct HealScreenView {
    bool available{}, open{}, checking{}, ready{}, applying{}, done{};
    std::string skill, caster, message;
    uint32_t target{}, caster_id{};
    int hp{}, max_hp{}, cost{};
};

// Owned by one asset worker. Composition and all asset IO/rasterization happen off sampling.
class ScreenComposer {
public:
    ScreenComposer();
    ~ScreenComposer();
    ScreenComposer(ScreenComposer&&) noexcept;
    ScreenComposer& operator=(ScreenComposer&&) noexcept;
    ScreenComposer(const ScreenComposer&)=delete;
    ScreenComposer& operator=(const ScreenComposer&)=delete;
    // Same native materials and fonts as the accepted companion pages. No game
    // actions or settings values are inferred from the artwork.
    std::optional<Image> Glyph(const RangeReader& read, bool player,
                               std::array<u8,4> color={255,255,255,255});
    // Latest map picture (native resolution, pane aspect, revealed paths only).
    // Map picture for this snapshot (native resolution, pane aspect, revealed paths only, padded
    // to at least the fixed-scale view); fills its geometry.
    // frame: world points (revealed icons) the picture must also contain.
    std::optional<Image> MapPicture(const RangeReader& read,const ExplorationSnapshot& map,MapGeometry* geometry,
                                    const std::vector<MapPoint>& frame={});
    // One minimap icon as the native painter draws it: background frame + frame, each tinted.
    std::optional<Image> IconGlyph(const RangeReader& read,const MinimapIcon& icon);
    // Native notification ring (soft half), alpha from brightness, tinted: the quest ripple.
    // Frame k of the native two-ring quest ripple (white, RingFrameSize px, rings half a period
    // apart, growing 0.5x..2.2x of 96 px and fading); RingFrames frames per RingPeriodMs.
    std::optional<Image> RingFrame(const RangeReader& read,u32 frame);
    // The Quests page's rows (all of them; the runtime scrolls the image).
    std::optional<Image> QuestList(const RangeReader& read,const QuestStates& states,int index);
    int QuestCount(const RangeReader& read,const QuestStates& states);
    std::optional<Image> RingGlyph(const RangeReader& read,std::array<u8,4> color);
    std::optional<Image> ComposeSettings(const RangeReader& read, bool hide_hud,
                                         bool haptics, bool reduced_motion,
                                         std::string* why=nullptr);
    std::optional<Image> Compose(const RangeReader& read,const Snapshot& snapshot,
                                 int selected,std::string* why=nullptr,
                                 const ExplorationSnapshot* map=nullptr,bool map_page=false,
                                 const ProtagonistAppearance* appearance=nullptr,
                                 const BattleSnapshot* battle=nullptr,int focus=0,
                                 bool modal_active=false,
                                 const ShopComparisonSnapshot* shop=nullptr,
                                 ScreenVisuals* visuals=nullptr,
                                 const MinimapIcons* icons=nullptr,
                                 const BattleDetailView* detail=nullptr,
                                 const HealScreenView* heal=nullptr,
                                 const LastBattle* last=nullptr,
                                 const MemberStats* stats=nullptr,
                                 const PartyView* party_view=nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ot0
