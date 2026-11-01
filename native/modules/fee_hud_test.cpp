// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// fee_hud decision logic against a fake mailbox and a simulated guest stub (no game data).

#include "fee_hud.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <chrono>
#include <thread>
#include <vector>

using namespace fee_hud;

namespace {

constexpr u64 Base = 0x8000000;

struct Fake {
    std::vector<unsigned char> box = std::vector<unsigned char>(mb::Size, 0);
    struct Done {
        u64 fn;
        std::array<u64, 4> a;
    };
    std::vector<Done> calls;
    u64 result = 0;
    std::map<u64, u64> mem;                      // guest u64 words for io.read
    std::function<u64(const Done&)> answer;      // per-call result (overrides `result`)

    u32 L32(u32 o) const {
        u32 v;
        std::memcpy(&v, &box[o], 4);
        return v;
    }
    u64 L64(u32 o) const {
        u64 v;
        std::memcpy(&v, &box[o], 8);
        return v;
    }
    void S32(u32 o, u32 v) {
        std::memcpy(&box[o], &v, 4);
    }
    // one game frame of the call stub
    void Frame() {
        S32(mb::Beat, L32(mb::Beat) + 1);
        const u32 s = L32(mb::Seq);
        if (s == L32(mb::Taken))
            return;
        S32(mb::Taken, s);
        if (const u64 fn = L64(mb::Fn)) {
            calls.push_back({fn - Base,
                             {L64(mb::Args), L64(mb::Args + 8), L64(mb::Args + 16), L64(mb::Args + 24)}});
            const u64 r = answer ? answer(calls.back()) : result;
            std::memcpy(&box[mb::Result], &r, 8);
        }
        S32(mb::Done, s);
    }
    Io MakeIo() {
        Io io;
        io.main_base = Base;
        io.read = [this](u64 a, void* out, std::size_t n) {
            const auto it = mem.find(a);
            if (it == mem.end() || n != 8)
                return false;
            std::memcpy(out, &it->second, 8);
            return true;
        };
        io.mailbox = 0x9000000;
        io.load32 = [this](u32 o, u32* v) { *v = L32(o); return true; };
        io.load64 = [this](u32 o, u64* v) { *v = L64(o); return true; };
        io.store32 = [this](u32 o, u32 v) { S32(o, v); return true; };
        io.store64 = [this](u32 o, u64 v) { std::memcpy(&box[o], &v, 8); return true; };
        return io;
    }
};

void Run(Hider& h, Fake& f, const Scene& sc, std::array<bool, ElementCount> show, bool aux,
         int ticks) {
    for (int i = 0; i < ticks; ++i) {
        h.Tick(sc, show, aux);
        f.Frame();
    }
}

void TestTerrainHideRestore() {
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    assert(h.Ready());
    Scene sc;
    sc.inst[TerrainL] = 0x1000;
    sc.seed[TerrainL] = 1;
    Run(h, f, sc, {}, true, 4);
    assert(f.L32(mb::Hide + 0) == 1);                // terrain hide word set
    assert(f.L64(mb::Target) == 0x1000);              // gate knows the Left instance
    assert(f.L32(mb::Want) == 1);                     // seeded from m_IsShow && m_IsEventShow
    assert(f.calls.size() == 1 && f.calls[0].fn == rva::TerrainRaw);
    assert(f.calls[0].a[0] == 0x1000 && f.calls[0].a[1] == 0);
    assert(h.Applied()[TerrainL] == 0x1000);
    // stays hidden: no more calls
    Run(h, f, sc, {}, true, 10);
    assert(f.calls.size() == 1);
    // setting re-shows: word cleared, game's wanted state restored
    Run(h, f, sc, {true, false, false, false}, true, 4);
    assert(f.L32(mb::Hide + 0) == 0);
    assert(f.calls.size() == 2 && f.calls[1].fn == rva::TerrainRaw && f.calls[1].a[1] == 1);
    assert(h.Applied()[TerrainL] == 0);
}

void TestRestoreRespectsGameWant() {
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    Scene sc;
    sc.inst[TerrainR] = 0x2000;
    sc.seed[TerrainR] = 1;
    Run(h, f, sc, {}, true, 4);
    assert(f.calls.size() == 1);
    f.S32(mb::Want + 4, 0); // the game asked to hide Right meanwhile (gate recorded it)
    Run(h, f, sc, {}, false, 4); // second screen hidden -> restore pass
    assert(f.L32(mb::Hide + 0) == 0);
    assert(f.calls.size() == 1); // nothing to show back
    assert(h.Applied()[TerrainR] == 0);
}

void TestUnitSeedsFirst() {
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    Scene sc;
    sc.inst[UnitL] = 0x3000;
    f.result = 1; // IsActive() -> true
    Run(h, f, sc, {}, true, 6);
    assert(f.calls.size() == 2);
    assert(f.calls[0].fn == rva::UnitIsActive && f.calls[0].a[0] == 0x3000);
    assert(f.calls[1].fn == rva::UnitRaw && f.calls[1].a[1] == 0);
    assert(f.L32(mb::Want + 8) == 1);
    assert(f.L64(mb::Target + 16) == 0x3000);
}

void TestInstanceChangeAndBattleEnd() {
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    Scene sc;
    sc.inst[MiniMapSlot] = 0x4000;
    Run(h, f, sc, {}, true, 4);
    assert(f.calls.size() == 1 && f.calls[0].fn == rva::MiniMapSetDirty);
    assert(f.L32(mb::Hide + 8) == 1);
    sc.inst[MiniMapSlot] = 0; // battle over: object gone, nothing to restore
    Run(h, f, sc, {}, true, 4);
    assert(f.calls.size() == 1 && h.Applied()[MiniMapSlot] == 0);
    sc.inst[MiniMapSlot] = 0x5000; // next battle
    Run(h, f, sc, {}, true, 4);
    assert(f.calls.size() == 2);
    // setting on -> word cleared before the re-apply call
    Run(h, f, sc, {false, false, true, false}, true, 4);
    assert(f.L32(mb::Hide + 8) == 0 && f.calls.size() == 3 && f.calls[2].fn == rva::MiniMapSetDirty);
}

void TestHubNeverHidden() {
    // The HubMiniMap.Hide gate word is never set; without live map objects nothing is called
    // beyond the map-mode poll.
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    Scene sc;
    sc.inst[HubSlot] = 0x6000;
    Run(h, f, sc, {}, true, 6);
    assert(f.L32(mb::Hide + 12) == 0);
    for (const auto& c : f.calls)
        assert(c.fn == rva::HubGetMapMode);
}

void TestSomnielHide() {
    // Small map shown (mode 1): poll mode -> activeSelf(m_mapS) -> SetActive(m_mapS, false);
    // then the setting re-shows it with SetActive(m_mapS, true). m_mapL never touched.
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    const u64 hub = 0x6000, map_s = 0x7000, map_l = 0x7100;
    f.mem[hub + rva::HubMapS] = map_s;
    f.mem[hub + rva::HubMapL] = map_l;
    f.mem[map_s + 0x10] = 0xAAAA; // native object alive
    f.mem[map_l + 0x10] = 0xBBBB;
    bool shown = true;
    f.answer = [&](const Fake::Done& c) -> u64 {
        if (c.fn == rva::HubGetMapMode)
            return 1;
        if (c.fn == rva::GoActiveSelf)
            return shown ? 1 : 0;
        if (c.fn == rva::GoSetActive)
            shown = c.a[1] != 0;
        return 0;
    };
    Scene sc;
    sc.inst[HubSlot] = hub;
    Run(h, f, sc, {}, true, 30);
    assert(!shown && h.SomnielHidden() == map_s);
    for (const auto& c : f.calls) {
        assert(c.a[0] != map_l);
        assert(c.fn != rva::HubShow && c.fn != rva::HubHideRaw); // never the whole GameObject
    }
    // the game shows it again (ZR mode change): the next poll hides it again
    shown = true;
    Run(h, f, sc, {}, true, 30);
    assert(!shown);
    // setting on: restored once, then idle
    Run(h, f, sc, {false, false, false, true}, true, 30);
    assert(shown && h.SomnielHidden() == 0);
    const std::size_t n = f.calls.size();
    Run(h, f, sc, {false, false, false, true}, true, 30);
    assert(f.calls.size() == n);
}

void TestShutdownDestroyedSomnielMap() {
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    const u64 hub = 0x6000, map = 0x7000;
    f.mem[hub + rva::HubMapS] = map;
    f.mem[map + 0x10] = 0xAAAA;
    f.answer = [](const Fake::Done& c) -> u64 {
        return c.fn == rva::HubGetMapMode || c.fn == rva::GoActiveSelf ? 1 : 0;
    };
    Scene sc;
    sc.inst[HubSlot] = hub;
    Run(h, f, sc, {}, true, 30);
    assert(h.SomnielHidden() == map);
    f.mem[map + 0x10] = 0; // Unity destroys the map before the next module tick
    const u32 seq = f.L32(mb::Seq);
    h.Shutdown();
    assert(f.L32(mb::Seq) == seq); // no SetActive request on the destroyed wrapper
    for (const auto& c : f.calls)
        assert(c.fn != rva::GoSetActive || c.a[1] == 0);
}

void TestGameCallQueue() {
    // A read-only game call waits for the HUD's own calls, then returns x0 and the scratch area.
    Fake f;
    Hider h;
    h.SetIo(f.MakeIo());
    assert(h.ScratchAddr(8) == 0x9000000 + mb::Scratch + 8);
    assert(h.Submit({0x1234, {7, h.ScratchAddr(), 0, 0}, 42}));
    assert(h.CallBusy());
    f.answer = [&](const Fake::Done& c) -> u64 {
        if (c.fn == 0x1234) {
            const float p[3] = {1.5f, 2.f, -3.f};
            std::memcpy(&f.box[mb::Scratch], p, sizeof p);
            return 99;
        }
        return 0;
    };
    Scene sc;
    Run(h, f, sc, {}, true, 3);
    const auto r = h.TakeResult();
    assert(r && r->tag == 42 && r->x0 == 99);
    float p[3];
    std::memcpy(p, r->scratch.data(), sizeof p);
    assert(p[0] == 1.5f && p[2] == -3.f);
    assert(!h.CallBusy() && !h.TakeResult());
}

void TestGameCallTimeout() {
    // A call the stub never serves is cancelled after the timeout and handed back as lost, so a
    // caller that gates its next request on the answer is not stuck forever.
    Fake g;
    Hider k;
    k.SetIo(g.MakeIo());
    assert(k.Submit({0x1234, {0, 0, 0, 0}, 0x105}));
    Scene sc;
    std::optional<GameResult> r;
    for (int i = 0; i < 700 && !r; ++i) {
        k.Tick(sc, {}, true);
        r = k.TakeResult();
    }
    assert(r && r->lost && r->tag == 0x105);
    assert(g.L64(mb::Fn) == 0); // cancelled: the stub skips it if it ever wakes up
    assert(!k.CallBusy());
}

void TestNoMailbox() {
    Hider h;
    h.SetIo(Io{});
    assert(!h.Ready());
    h.Tick(Scene{}, {}, true);
    h.Shutdown();
}

} // namespace

int main() {
    TestTerrainHideRestore();
    TestRestoreRespectsGameWant();
    TestUnitSeedsFirst();
    TestInstanceChangeAndBattleEnd();
    TestHubNeverHidden();
    TestSomnielHide();
    TestShutdownDestroyedSomnielMap();
    TestGameCallQueue();
    TestGameCallTimeout();
    TestNoMailbox();
    std::puts("fee_hud tests passed");
    return 0;
}
