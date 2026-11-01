// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Fire Emblem Engage (Ver. 2.0.0): hide the native top-screen HUD the companion shows better
// (terrain window, unit window strip, battle minimap; the Somniel minimap is NOT hidden -- see
// Hider::Tick). Defaults (owner decision 2026-11-01): the terrain and unit windows stay SHOWN on
// the top screen, the battle and Somniel minimaps are hidden; the companion settings toggle each
// one ("@flag:top.terrain2" / "top.unit2" / "top.minimap" / "top.somniel", see ReadSettings).
//
// Mechanism (tools/fire-emblem-engage/gen_load_plan.py): the load plan puts a gate in front of
// the game's own visibility setters. While an element's hide word is set, a request to show it
// becomes a request to hide it, and the value the game asked for is kept in the mailbox. This
// module only (a) sets the hide words, (b) tells the gates which instances are the HUD's, and (c)
// once per change runs one of the game's own functions through the mailbox call stub to hide an
// element that is already on screen, or to restore what the game asked for. No game memory is
// written outside the mailbox.
//
// Restore: setting on, second screen hidden, module destroyed -> hide words cleared and every
// element the module hid gets the game's own last requested state back. Battle end / scene change
// destroys the objects; new ones are gated from their first show.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace fee_hud {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using u8 = std::uint8_t;

// mailbox layout (gen_load_plan.py)
namespace mb {
constexpr u32 Seq = 0x00, Taken = 0x04, Beat = 0x08, Done = 0x0C, Fn = 0x10, Args = 0x18,
              Result = 0x38;
constexpr u32 Hide = 0x40;   // u32 x4: terrain, unit, minimap, hub
constexpr u32 Target = 0x50; // u64 x4: terrain L/R, unit L/R
constexpr u32 Want = 0x70;   // u32 x5: terrain L/R, unit L/R, hub
constexpr u32 Forced = 0x84; // u32 forced-hide counter
constexpr u32 Scratch = 0x100; // 0x100 bytes of out-parameter space for read-only game calls
constexpr u32 ScratchSize = 0x100;
constexpr u32 Size = 0x1000;
} // namespace mb

// main RVAs (Ver. 2.0.0; raw entries are the load plan's, gen_load_plan.py --print-offsets)
namespace rva {
constexpr u64 TerrainRaw = 0x3F47108;  // MapTerrainInfoSingle.SetVisible(this, bool), ungated
constexpr u64 UnitRaw = 0x3F47168;     // UnitInfoWindow.SetActive(this, bool), ungated
constexpr u64 HubHideRaw = 0x3F471E4;  // HubMiniMap.Hide(this), want untouched
constexpr u64 HubShow = 0x28BA270;     // HubMiniMap.Show(this)
constexpr u64 UnitIsActive = 0x1FA5AC0; // UnitInfoWindow.IsActive(this) -> bool
constexpr u64 MiniMapSetDirty = 0x1DF66D0; // MapMiniMap.SetDirty() (static): re-applies m_MiniMapMode
// Somniel: only the minimap's own map objects (HubMiniMap.m_mapS +0x88 small corner map,
// m_mapL +0xB8 large overlay; SetMode(1/2) 0x28BA100 toggles exactly these two), never the
// HubMiniMap GameObject (that one also carries NPCs and markers).
constexpr u64 HubGetMapMode = 0x28BA080;   // HubMiniMap.get_CurrentMapMode(this): 0 none 1 S 2 L
constexpr u64 GoActiveSelf = 0x2C4EA60;    // GameObject.get_activeSelf(this)
constexpr u64 GoSetActive = 0x2C4EA10;     // GameObject.SetActive(this, bool)
constexpr u64 HubMapS = 0x88, HubMapL = 0xB8;
// TypeInfo slots
constexpr u64 TiTerrain = 0x6153038; // SingletonClass<MapTerrainInfo>
constexpr u64 TiUnitInfo = 0x6153678; // SingletonProcInst<UnitInfo>
constexpr u64 TiMiniMap = 0x61534E0; // SingletonProcInst<MapMiniMap>
constexpr u64 TiHub = 0x6153490;     // SingletonProcInst<HubSequence>
} // namespace rva

enum Element : int { Terrain, Unit, MiniMap, Hub, ElementCount };
// One slot per hideable instance.
enum Slot : int { TerrainL, TerrainR, UnitL, UnitR, MiniMapSlot, HubSlot, SlotCount };
constexpr Element ElementOf(int slot) {
    return slot <= TerrainR ? Terrain : slot <= UnitR ? Unit : slot == MiniMapSlot ? MiniMap : Hub;
}
constexpr u32 Unknown = 0xFFFFFFFFu;

/// Guest memory + mailbox access (host-backed in the module, fake in the test).
struct Io {
    std::function<bool(u64, void*, std::size_t)> read;
    std::function<bool(u32, u32*)> load32;
    std::function<bool(u32, u64*)> load64;
    std::function<bool(u32, u32)> store32;
    std::function<bool(u32, u64)> store64;
    u64 main_base = 0;
    u64 mailbox = 0; // guest address of the mailbox (out-parameter pointers for game calls)
};

/// One read-only call to the game's own code through the same call stub (fn = main RVA). Args
/// that need a guest out-pointer use ScratchAddr(); the stub serves one call per frame, HUD
/// calls first.
struct GameCall {
    u64 fn = 0;
    std::array<u64, 4> args{};
    u32 tag = 0; // caller's id, echoed in the result
};
struct GameResult {
    u32 tag = 0;
    u64 x0 = 0;
    std::array<u8, mb::ScratchSize> scratch{}; // the scratch area right after the call
    bool lost = false; // the stub never served it (timed out and cancelled): no x0/scratch
};

/// What the HUD looks like this tick (resolved from the game's singletons).
struct Scene {
    std::array<u64, SlotCount> inst{}; // 0 = not live (MiniMapSlot: the MapMiniMap singleton)
    std::array<u32, SlotCount> seed{Unknown, Unknown, Unknown, Unknown, Unknown, Unknown};
};

class Hider {
public:
    void SetIo(Io io_);
    bool Ready() const {
        return ready;
    }
    /// show[e] = the setting re-shows element e; aux = second screen present.
    void Tick(const Scene& scene, const std::array<bool, ElementCount>& show, bool aux);
    /// Resolve the HUD instances from game memory.
    Scene Resolve() const;
    /// Clear every hide word and restore what the module hid (bounded wait for the game thread).
    void Shutdown();
    std::array<u64, SlotCount> Applied() const {
        return applied;
    }

    /// Queue one game call (replaces a queued, not yet issued one). False when not ready.
    bool Submit(const GameCall& call);
    /// True while a submitted call is queued or in flight.
    bool CallBusy() const {
        return queued.fn != 0 || pending.kind == Call::Extra;
    }
    /// The last finished game call (once).
    std::optional<GameResult> TakeResult();
    /// Guest address of mb::Scratch + offset (0 when unknown).
    u64 ScratchAddr(u32 offset = 0) const {
        return io.mailbox ? io.mailbox + mb::Scratch + offset : 0;
    }

private:
    struct Call {
        int slot = -1;
        enum Kind { None, Hide, Restore, Seed, Extra, SomMode, SomActive, SomSet } kind = None;
        u64 inst = 0;
        u32 tag = 0;
    };
    bool Issue(const Call& c, u64 fn, std::array<u64, 4> args);
    bool Poll(); // true when no call is in flight
    bool Plan(int slot, const Scene& scene, bool hide);
    u32 Want(int slot) const;

    Io io;
    bool ready = false;
    std::array<u32, ElementCount> hide_word{};
    std::array<u64, SlotCount> target{}; // what the gates know
    std::array<u64, SlotCount> applied{}; // instances the module hid
    bool IssueExtra();
    /// Somniel minimap hide: poll the map mode, hide the matching map object, restore it.
    bool TickSomniel(u64 hub, bool hide);
    struct Somniel {
        u64 hub = 0;
        int mode = -1;     // last get_CurrentMapMode
        u64 target = 0;    // m_mapS / m_mapL for that mode
        u64 hidden = 0;    // object this module deactivated (restore target)
        int wait = 0;      // ticks until the next poll
        int step = 0;      // 0 mode, 1 activeSelf, 2 set
        bool active = false;
    } som;
public:
    /// Somniel minimap objects this module currently keeps hidden (0 = none).
    u64 SomnielHidden() const {
        return som.hidden;
    }
private:
    Call pending;
    GameCall queued;
    std::optional<GameResult> result_;
    u32 seq = 0;
    int wait_ticks = 0;
};

/// Reads "@flag:top.*" (fallback 0 = hidden) and "__aux_present".
std::array<bool, ElementCount> ReadSettings(const EdenDsmodHostApi& host);
/// Host-backed Io; empty (Ready() false) without a mailbox of mb::Size.
Io MakeIo(const EdenDsmodHostApi& host, const EdenDsmodHostExtensions* ext);

} // namespace fee_hud
