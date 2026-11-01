// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_hud.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace fee_hud {
namespace {

constexpr int CallTimeoutTicks = 600; // ~10 s of host ticks without the stub serving the call

constexpr u64 RawFor(int slot) {
    return ElementOf(slot) == Terrain ? rva::TerrainRaw : rva::UnitRaw;
}
constexpr u32 TargetOffset(int slot) {
    return mb::Target + 8u * static_cast<u32>(slot); // TerrainL..UnitR are slots 0..3
}
constexpr u32 WantOffset(int slot) {
    return mb::Want + 4u * static_cast<u32>(slot == HubSlot ? 4 : slot);
}
constexpr bool Gated(int slot) {
    return slot <= UnitR;
}

} // namespace

void Hider::SetIo(Io io_) {
    io = std::move(io_);
    ready = io.read && io.load32 && io.load64 && io.store32 && io.store64 && io.main_base;
    if (!ready)
        return;
    // A fresh module never trusts a previous instance's words (reload after a crash).
    for (int e = 0; e < ElementCount; ++e)
        io.store32(mb::Hide + 4u * e, 0);
    for (int s = 0; s <= UnitR; ++s) {
        io.store32(WantOffset(s), Unknown);
        io.store64(TargetOffset(s), 0);
    }
    io.store32(WantOffset(HubSlot), Unknown);
    u32 cur = 0;
    if (io.load32(mb::Seq, &cur))
        seq = cur;
}

u32 Hider::Want(int slot) const {
    u32 v = Unknown;
    if (!io.load32 || !io.load32(WantOffset(slot), &v))
        return Unknown;
    return v;
}

bool Hider::Issue(const Call& c, u64 fn, std::array<u64, 4> args) {
    u32 current = 0, served = 0;
    if (!io.load32(mb::Seq, &current) || !io.load32(mb::Done, &served) || served != current)
        return false; // the stub still owns the previous request
    bool ok = io.store64(mb::Fn, io.main_base + fn);
    for (u32 k = 0; k < 4; ++k)
        ok = ok && io.store64(mb::Args + 8 * k, args[k]);
    seq = current + 1;
    ok = ok && io.store32(mb::Seq, seq); // last: publishes the request
    if (ok) {
        pending = c;
        wait_ticks = 0;
    }
    return ok;
}

bool Hider::Poll() {
    if (pending.kind == Call::None)
        return true;
    u32 done = 0;
    if (io.load32(mb::Done, &done) && done == seq) {
        u64 result = 0;
        io.load64(mb::Result, &result);
        const int s = pending.slot;
        switch (pending.kind) {
        case Call::Hide:
            applied[s] = pending.inst;
            break;
        case Call::Restore:
            applied[s] = 0;
            break;
        case Call::Seed:
            if (Want(s) == Unknown)
                io.store32(WantOffset(s), static_cast<u32>(result & 1));
            break;
        case Call::SomMode:
            som.mode = int(result & 0xFFFFFFFFu);
            som.step = 1;
            break;
        case Call::SomActive:
            som.active = (result & 1) != 0;
            som.step = 2;
            break;
        case Call::SomSet:
            som.hidden = pending.inst; // inst = the object hidden (0 after a restore)
            som.step = 0;
            break;
        case Call::Extra: {
            GameResult r;
            r.tag = pending.tag;
            r.x0 = result;
            for (u32 k = 0; k < mb::ScratchSize; k += 8) {
                u64 v = 0;
                io.load64(mb::Scratch + k, &v);
                std::memcpy(r.scratch.data() + k, &v, 8);
            }
            result_ = r;
            break;
        }
        case Call::None:
            break;
        }
        pending = {};
        return true;
    }
    if (++wait_ticks > CallTimeoutTicks) {
        io.store64(mb::Fn, 0); // the stub skips a cancelled request
        if (pending.kind == Call::Extra) { // tell the caller, so it can ask again later
            GameResult r;
            r.tag = pending.tag;
            r.lost = true;
            result_ = r;
        }
        pending = {};
    }
    return false;
}

bool Hider::Plan(int slot, const Scene& scene, bool hide) {
    const u64 inst = scene.inst[slot];
    if (applied[slot] && applied[slot] != inst)
        applied[slot] = 0; // destroyed (battle end / scene change) or replaced: nothing to restore
    if (hide && inst && applied[slot] != inst) {
        switch (ElementOf(slot)) {
        case Terrain:
            if (Want(slot) == Unknown && scene.seed[slot] != Unknown)
                io.store32(WantOffset(slot), scene.seed[slot]);
            return Issue({slot, Call::Hide, inst}, rva::TerrainRaw, {inst, 0, 0, 0});
        case Unit:
            if (Want(slot) == Unknown)
                return Issue({slot, Call::Seed, inst}, rva::UnitIsActive, {inst, 0, 0, 0});
            return Issue({slot, Call::Hide, inst}, rva::UnitRaw, {inst, 0, 0, 0});
        case MiniMap:
            return Issue({slot, Call::Hide, inst}, rva::MiniMapSetDirty, {0, 0, 0, 0});
        case Hub:
            if (Want(slot) == Unknown) // no show/hide seen yet: the free-roam default is shown
                io.store32(WantOffset(slot), 1);
            return Issue({slot, Call::Hide, inst}, rva::HubHideRaw, {inst, 0, 0, 0});
        default:
            return false;
        }
    }
    if (!hide && applied[slot] && applied[slot] == inst) {
        if (ElementOf(slot) == MiniMap)
            return Issue({slot, Call::Restore, inst}, rva::MiniMapSetDirty, {0, 0, 0, 0});
        if (Want(slot) != 1) {
            applied[slot] = 0; // the game itself wants it hidden now
            return false;
        }
        if (ElementOf(slot) == Hub)
            return Issue({slot, Call::Restore, inst}, rva::HubShow, {inst, 0, 0, 0});
        return Issue({slot, Call::Restore, inst}, RawFor(slot), {inst, 1, 0, 0});
    }
    return false;
}

bool Hider::Submit(const GameCall& call) {
    if (!ready || !call.fn)
        return false;
    queued = call;
    return true;
}

std::optional<GameResult> Hider::TakeResult() {
    auto r = result_;
    result_.reset();
    return r;
}

bool Hider::IssueExtra() {
    if (!queued.fn)
        return false;
    for (u32 k = 0; k < mb::ScratchSize; k += 8)
        io.store64(mb::Scratch + k, 0);
    Call c;
    c.kind = Call::Extra;
    c.tag = queued.tag;
    const bool ok = Issue(c, queued.fn, queued.args);
    if (ok)
        queued = {};
    return ok;
}

void Hider::Tick(const Scene& scene, const std::array<bool, ElementCount>& show, bool aux) {
    if (!ready || !Poll())
        return;
    std::array<bool, ElementCount> hide{};
    for (int e = 0; e < ElementCount; ++e)
        hide[e] = aux && !show[e] && e != Hub;
    // Hub: never through HubMiniMap.Hide() (that GameObject also carries Somniel NPCs, facility
    // markers and effects -- live 2026-10-30); its gates stay inert. TickSomniel hides only the
    // minimap's own map object instead (m_mapS / m_mapL, the two SetMode toggles).
    // Tell the gates which instances are the HUD's; a new instance's wanted state is unknown.
    for (int s = 0; s < SlotCount; ++s) {
        if (scene.inst[s] == target[s])
            continue;
        if (s == HubSlot || Gated(s))
            io.store32(WantOffset(s), Unknown);
        if (Gated(s))
            io.store64(TargetOffset(s), scene.inst[s]);
        target[s] = scene.inst[s];
    }
    // Hide words first (so the game's own show calls are gated before the module's hide call);
    // cleared before any restore call.
    for (int e = 0; e < ElementCount; ++e) {
        const u32 w = hide[e] ? 1u : 0u;
        if (hide_word[e] != w && io.store32(mb::Hide + 4u * e, w))
            hide_word[e] = w;
    }
    for (int s = 0; s < SlotCount; ++s)
        if (Plan(s, scene, hide[ElementOf(s)]))
            return; // one call per tick
    if (TickSomniel(scene.inst[HubSlot], aux && !show[Hub]))
        return;
    IssueExtra(); // HUD idle: the stub is free for one read-only game call
}

bool Hider::TickSomniel(u64 hub, bool hide) {
    auto r64 = [&](u64 a) -> u64 {
        u64 v = 0;
        return a && io.read(a, &v, sizeof v) ? v : 0;
    };
    if (hub != som.hub) {
        som = {};
        som.hub = hub; // a new Somniel scene: nothing hidden in it yet
    }
    if (!hub)
        return false;
    const auto alive = [&](u64 go) { return go && r64(go + 0x10) != 0; };
    if (!hide && !som.hidden)
        return false;
    if (som.step == 0) {
        if (som.wait-- > 0)
            return false;
        som.wait = 8; // poll about seven times a second
        return Issue({-1, Call::SomMode, hub}, rva::HubGetMapMode, {hub, 0, 0, 0});
    }
    som.target = som.mode == 1 ? r64(hub + rva::HubMapS) : som.mode == 2 ? r64(hub + rva::HubMapL) : 0;
    if (!alive(som.target)) {
        som.step = 0;
        if (!hide)
            som.hidden = 0; // mode None / destroyed: the game shows nothing to restore
        return false;
    }
    if (!hide) {
        // Restore: give the current mode's map object back (the game's SetMode state).
        som.step = 0;
        Call c{-1, Call::SomSet, 0};
        return Issue(c, rva::GoSetActive, {som.target, 1, 0, 0});
    }
    if (som.step == 1)
        return Issue({-1, Call::SomActive, som.target}, rva::GoActiveSelf, {som.target, 0, 0, 0});
    // step 2: hide it when the game has it shown
    som.step = 0;
    if (!som.active) {
        if (!som.hidden)
            som.hidden = som.target; // already hidden (by us earlier, or this tick's re-check)
        return false;
    }
    return Issue({-1, Call::SomSet, som.target}, rva::GoSetActive, {som.target, 0, 0, 0});
}

void Hider::Shutdown() {
    if (!ready)
        return;
    for (int e = 0; e < ElementCount; ++e) {
        io.store32(mb::Hide + 4u * e, 0);
        hide_word[e] = 0;
    }
    using Clock = std::chrono::steady_clock;
    const auto until = Clock::now() + std::chrono::milliseconds(400);
    while (Clock::now() < until) {
        bool any = false;
        for (const u64 a : applied)
            any = any || a;
        if (!any && pending.kind == Call::None)
            break;
        if (Poll()) {
            const Scene scene = Resolve();
            bool issued = false;
            for (int s = 0; s < SlotCount && !issued; ++s)
                issued = Plan(s, scene, false);
            if (!issued) {
                bool left = false;
                for (const u64 a : applied)
                    left = left || a;
                if (!left)
                    break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Somniel minimap: give back the object this module deactivated (bounded wait).
    if (som.hidden) {
        const auto until2 = Clock::now() + std::chrono::milliseconds(200);
        while (Clock::now() < until2 && !Poll())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        u64 native = 0;
        // A scene can be destroyed between the last tick and module unload. Unity getters and
        // SetActive throw on destroyed wrappers; never restore one without a live native object.
        if (pending.kind == Call::None &&
            io.read(som.hidden + 0x10, &native, sizeof native) && native) {
            Call c{-1, Call::SomSet, 0};
            if (Issue(c, rva::GoSetActive, {som.hidden, 1, 0, 0}))
                while (Clock::now() < until2 && !Poll())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (pending.kind != Call::None)
        io.store64(mb::Fn, 0);
    pending = {};
}

// ---- resolution ---------------------------------------------------------------------------

Scene Hider::Resolve() const {
    Scene sc;
    if (!ready)
        return sc;
    auto r64 = [&](u64 a) -> u64 {
        u64 v = 0;
        return a && io.read(a, &v, sizeof v) ? v : 0;
    };
    auto r8 = [&](u64 a) -> unsigned {
        unsigned char v = 0;
        return a && io.read(a, &v, 1) ? v : 0u;
    };
    auto singleton = [&](u64 slot) -> u64 {
        const u64 klass = r64(io.main_base + slot);
        const u64 statics = klass ? r64(klass + 0xB8) : 0;
        return statics ? r64(statics) : 0;
    };
    auto is_class = [&](u64 obj, const char* want) {
        const u64 klass = r64(obj);
        const u64 name = klass ? r64(klass + 0x10) : 0;
        char buf[24]{};
        const std::size_t n = std::strlen(want) + 1;
        return name && n <= sizeof buf && io.read(name, buf, n) && std::memcmp(buf, want, n) == 0;
    };
    auto unity_alive = [&](u64 wrapper) { return wrapper && r64(wrapper + 0x10) != 0; };
    auto element = [&](u64 array, int i) -> u64 {
        if (!array || r64(array + 0x18) <= static_cast<u64>(i))
            return 0;
        return r64(array + 0x20 + 8u * i);
    };

    // MapTerrainInfo.m_MapTerrainInfoSingles[Left/Right]
    if (const u64 info = singleton(rva::TiTerrain)) {
        const u64 singles = r64(info + 0x28);
        for (int i = 0; i < 2; ++i) {
            const u64 e = element(singles, i);
            if (!e || !is_class(e, "MapTerrainInfoSingle") || !unity_alive(r64(e + 0x10)))
                continue;
            const unsigned valid = r8(e + 0x30), show = r8(e + 0x31), event = r8(e + 0x32);
            if (!valid)
                continue;
            sc.inst[TerrainL + i] = e;
            sc.seed[TerrainL + i] = (show && event) ? 1u : 0u; // what SetVisible was last given
        }
    }
    // UnitInfo.m_Windows[Left/Right].m_UnitInfoWindowSimple
    if (const u64 info = singleton(rva::TiUnitInfo)) {
        const u64 windows = r64(info + 0x80);
        for (int i = 0; i < 2; ++i) {
            const u64 w = element(windows, i);
            const u64 win = w ? r64(w + 0x30) : 0;
            if (!win || !is_class(win, "UnitInfoWindow") || !r8(win + 0x38))
                continue;
            sc.inst[UnitL + i] = win;
        }
    }
    // MapMiniMap singleton with a live m_MiniMapObject
    if (const u64 mm = singleton(rva::TiMiniMap); mm && unity_alive(r64(mm + 0x80)))
        sc.inst[MiniMapSlot] = mm;
    // HubSequence.<MiniMap>k__BackingField
    if (const u64 hs = singleton(rva::TiHub)) {
        const u64 hub = r64(hs + 0x130);
        u32 bind = 1;
        io.read(hs + 0x5C, &bind, 4); // ProcInst.IsBind: menu / fast travel / departure running
        if (hub && is_class(hub, "HubMiniMap") && unity_alive(hub) && r8(hub + 0x28) && bind == 0)
            sc.inst[HubSlot] = hub; // only in free roam: objects die during a departure
    }
    return sc;
}

// ---- host glue ----------------------------------------------------------------------------

std::array<bool, ElementCount> ReadSettings(const EdenDsmodHostApi& host) {
    // Owner 2026-11-01: the unit and terrain windows stay on the top screen by default, the two
    // minimaps hidden. The shown-by-default flags carry a "2" so a value persisted under the old
    // hidden default (top.unit / top.terrain = 0 in <EdenDir>/dualscreen/persist) cannot hide them.
    static constexpr const char* Names[ElementCount] = {"@flag:top.terrain2", "@flag:top.unit2",
                                                        "@flag:top.minimap", "@flag:top.somniel"};
    static constexpr std::int64_t Defaults[ElementCount] = {1, 1, 0, 0};
    std::array<bool, ElementCount> show{};
    for (int e = 0; e < ElementCount; ++e)
        show[e] = host.get_i64 ? host.get_i64(host.userdata, Names[e], Defaults[e]) != 0 : Defaults[e] != 0;
    return show;
}

Io MakeIo(const EdenDsmodHostApi& host, const EdenDsmodHostExtensions* ext) {
    Io io;
    if (!ext || ext->mailbox_size < mb::Size || !ext->load_u32 || !ext->load_u64 ||
        !ext->store_u32 || !ext->store_u64 || !host.read_memory || !host.main_base)
        return io;
    const EdenDsmodHostApi h = host; // function pointers + userdata outlive the module
    const EdenDsmodHostExtensions x = *ext;
    io.main_base = h.main_base;
    io.mailbox = x.mailbox_address;
    io.read = [h](u64 a, void* out, std::size_t n) {
        return h.read_memory(h.userdata, a, out, n) == EDEN_DSMOD_TRUE;
    };
    io.load32 = [x](u32 off, u32* v) { return x.load_u32(x.userdata, off, v) == EDEN_DSMOD_TRUE; };
    io.load64 = [x](u32 off, u64* v) {
        std::uint64_t t = 0;
        const bool ok = x.load_u64(x.userdata, off, &t) == EDEN_DSMOD_TRUE;
        *v = t;
        return ok;
    };
    io.store32 = [x](u32 off, u32 v) { return x.store_u32(x.userdata, off, v) == EDEN_DSMOD_TRUE; };
    io.store64 = [x](u32 off, u64 v) { return x.store_u64(x.userdata, off, v) == EDEN_DSMOD_TRUE; };
    return io;
}

} // namespace fee_hud
