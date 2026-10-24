// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_warp_menu.h"
#include "echoes_warp_choice.h"
#include "echoes_player.h"
#include <functional>
namespace echoes {
using WarpZoomWrite = std::function<bool(const WarpMenuSnapshot &)>;
using WarpTargetWrite = std::function<bool(const WarpMenuSnapshot &,uint32_t)>;
enum class WarpPulse { None, Minus, A, B, Neutral, West, East, North, South, NorthWest, NorthEast, SouthWest, SouthEast };
enum class WarpPrepareStatus { Idle, Pending, Ready, Aborted };
enum class WarpPrepareAbort {
  None, InvalidRequest, SourceChanged, ContextChanged, Locked, NativeDenied,
  UnexpectedFocus, Interference, Cancelled, Clock, Timeout, UnsupportedTarget, Navigation
};
// Preparation opens the original map and navigates to an exact authenticated
// destination using native icon/cursor coordinates. With direct_target, one
// cursor write after a bounded native-input warmup replace directional traversal, followed by
// exact native focus readback without replay. Ready grants no travel
// completion: WarpTravelDrive separately authenticates the popup and arrival.
// A Minus result requests one bounded manifest pulse, then neutral; caller always
// neutralizes on cancel/abort/interference and must not automatically replay.
class WarpPrepareDrive {
public:
 static constexpr uint64_t PulseTicks=2, SettleTicks=6, RequestTimeout=720,
                           MaxClockGap=60, FarHoldTicks=60, NearHoldTicks=12, DirectRefreshTicks=8, DirectPostTicks=2,
                           ZoomTimeoutTicks=180;
 bool Request(const NativeWarpCatalog &,std::string_view warp_set,uint32_t ordinal,
              const ContextSnapshot &,const MapSnapshot &,const WarpUnlockSnapshot &,
              uint64_t tick,bool physical_interference=false,
              const WarpMenuSnapshot *already_open=nullptr);
 WarpPulse Advance(const ContextSnapshot &,const MapSnapshot &,
                   const WarpUnlockSnapshot &,const WarpMenuSnapshot &,
                   uint64_t tick,bool physical_interference=false,
                   const WarpTargetWrite &direct_target={},const WarpZoomWrite &full_zoom={});
 void Cancel();
 WarpPrepareStatus status()const{return status_;}
 WarpPrepareAbort abort_reason()const{return abort_;}
private:
 void Abort(WarpPrepareAbort);
 bool Source(const MapSnapshot &,const WarpUnlockSnapshot &)const;
 WarpPrepareStatus status_{WarpPrepareStatus::Idle};
 WarpPrepareAbort abort_{WarpPrepareAbort::None};
 MapEpochSource source_{};
 MapLevelSource level_source_{};
 std::string raw_level_,set_,flag_;
 uint32_t ordinal_{};
 uint64_t start_{},last_{},pulse_at_{},stable_at_{},map_component_{},focused_{};
 bool sent_{},focused_seen_{},target_seen_{},moving_{},neutralizing_{},direct_sent_{},direct_armed_{},zoom_sent_{};
 uint64_t zoom_ready_at_{};
 uint64_t move_until_{},neutral_until_{},target_component_{},native_view_{};
 float target_x_{},target_y_{},origin_x_{},origin_y_{};
 WarpPulse movement_{WarpPulse::None};
};
enum class WarpTravelStatus { Idle, Pending, Complete, Aborted };
enum class WarpTravelAbort {
 None, InvalidRequest, Preparation, SourceChanged, ContextChanged, Clock,
 Timeout, UnexpectedChoice, NativeDenied, Postcondition, Cancelled, Interference,
 // Zelda already stands within arrival range of the destination: travel there
 // could not be told apart from the map closing, so it is refused before confirm.
 AlreadyThere
};
// Full Field-only native route. An optional authenticated cursor-only writer
// replaces directional traversal; no guest calls or world/save writes. A and Minus are
// ONE-SHOT ButtonPulseFrames-frame pulses (manifest contract); analog outputs
// repeat per tick for a bounded hold. StableTicks exceeds the button hold so
// opening and confirmation A pulses are separated by an observed release.
// RequestTimeout bounds pre-confirm navigation/dialogue. ArrivalTimeout starts
// at the final affirmative pulse; no more inputs are sent while awaiting HUD.
// Neutral always clears the owned LEFT stick. Caller cancels on physical input
// interference and neutralizes immediately on every abort/cancel/completion.
class WarpTravelDrive {
public:
 static constexpr uint64_t RequestTimeout=1536, ArrivalTimeout=3600, ReadbackTimeout=180,
                           MaxClockGap=60, StableTicks=12, ButtonPulseFrames=8;
 static_assert(StableTicks>=ButtonPulseFrames+2);
 // Arrival range in native map-layout units; a start within SameSpotLayout of
 // the destination is refused before confirm.
 static constexpr float ArrivalLayout=6.0f, SameSpotLayout=8.0f;
 bool Request(const NativeWarpCatalog &,std::string_view set,uint32_t ordinal,
              const ContextSnapshot &,const MapSnapshot &,const WarpUnlockSnapshot &,
              const WarpMenuSnapshot &,uint64_t tick,bool physical_interference=false);
 WarpPulse Advance(const ContextSnapshot &,const MapSnapshot &,const WarpUnlockSnapshot &,
                   const WarpMenuSnapshot &,const WarpChoiceSnapshot &,
                   const FieldPlayerProjection &,uint64_t tick,bool physical_interference=false,
                   const WarpTargetWrite &direct_target={},const WarpZoomWrite &full_zoom={});
 void Cancel();
 WarpTravelStatus status()const{return status_;}
 WarpTravelAbort abort_reason()const{return abort_;}
 WarpPrepareAbort preparation_abort()const{return prepare_.abort_reason();}
private:
 enum class Phase { Prepare, WaitPopup, WaitCommit };
 void Abort(WarpTravelAbort);
 bool SaveSame(const MapSnapshot &)const;
 bool BeforeCommit(const ContextSnapshot &,const MapSnapshot &,const WarpUnlockSnapshot &,
                   const WarpMenuSnapshot &)const;
 WarpPrepareDrive prepare_;
 WarpTravelStatus status_{WarpTravelStatus::Idle};
 WarpTravelAbort abort_{WarpTravelAbort::None};
 Phase phase_{Phase::Prepare};
 NativeWarpDestination destination_;
 MapEpochSource source_{};
 uint32_t ordinal_{};
 uint64_t requested_{},last_{},phase_at_{},stable_at_{},menu_component_{},focused_{},popup_{};
 float target_x_{},target_y_{};
 bool popup_seen_{},execution_seen_{},departure_seen_{},world_seen_{},arrival_seen_{};
 PlayerWorldSnapshot initial_world_;
 float arrival_x_{},arrival_y_{};
};
} // namespace echoes
