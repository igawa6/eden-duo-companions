// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctr_reader.h"
#include <cassert>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>

namespace CtrReader {
struct ReaderTestAccess {
    struct Memory {
        std::unordered_map<std::uint64_t, std::uint8_t> bytes;
        bool fail_write{};
        int writes{};
        int page_probes{}; ///< heap pages the widget scan looked at
        template <class T> void Put(std::uint64_t at, T value) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t i = 0; i < sizeof value; ++i) bytes[at + i] = p[i];
        }
        static EdenDsmodBool Mapped(void* p, std::uint64_t at, std::uint64_t size) {
            auto& m = *static_cast<Memory*>(p);
            for (std::uint64_t i = 0; i < size; ++i)
                if (!m.bytes.contains(at + i)) return EDEN_DSMOD_FALSE;
            return EDEN_DSMOD_TRUE;
        }
        static EdenDsmodBool Read(void* p, std::uint64_t at, void* out, std::uint64_t size) {
            auto& m = *static_cast<Memory*>(p);
            if (!Mapped(p, at, size)) return EDEN_DSMOD_FALSE;
            for (std::uint64_t i = 0; i < size; ++i)
                static_cast<std::uint8_t*>(out)[i] = m.bytes.at(at + i);
            return EDEN_DSMOD_TRUE;
        }
        static EdenDsmodBool Write(void* p, std::uint64_t at, const void* in, std::uint64_t size) {
            auto& m = *static_cast<Memory*>(p);
            ++m.writes;
            if (m.fail_write || !Mapped(p, at, size)) return EDEN_DSMOD_FALSE;
            for (std::uint64_t i = 0; i < size; ++i)
                m.bytes.at(at + i) = static_cast<const std::uint8_t*>(in)[i];
            return EDEN_DSMOD_TRUE;
        }
    };
    static const std::uint8_t* ReadPointer(void* p, std::uint64_t, std::size_t) {
        ++static_cast<Memory*>(p)->page_probes;
        return nullptr; // fall back to is_mapped: the scanned heap is empty
    }
    static std::uint64_t HeapBegin(void*) { return 0x10000000; }
    static std::uint64_t HeapEnd(void*) { return 0x10000000 + (64ull << 20); }

    /// A race built while we held the minimap option off has no minimap widget: once the rest of
    /// the HUD is resolved the heap scan idles instead of rescanning for the whole race.
    static void ScanIdlesWithoutMinimap() {
        Memory memory;
        EdenDsmodHostApi host{};
        host.userdata = &memory;
        host.main_base = 0x100000;
        host.is_mapped = Memory::Mapped;
        host.read_memory = Memory::Read;
        host.write_memory = Memory::Write;
        host.get_read_pointer = ReadPointer;
        host.get_heap_begin = HeapBegin;
        host.get_heap_end = HeapEnd;
        CtrArt::Library art;
        Reader r{art};
        r.host = &host;
        r.static_variant = 0;
        const std::uint64_t placeable = host.main_base + 0x47EFF68;
        const auto node = [&](std::uint64_t at) { memory.Put<std::uint64_t>(at, placeable); };
        // the player's racer HUD: racer -> kart handle -> entity -> components -> CRacerHudComponent
        // -> project -> root -> composition -> Root_placeable children (timer, lap, position)
        constexpr std::uint64_t racer = 0x600000, kart = 0x601000, entity = 0x602000, comps = 0x603000,
                                items = 0x604000, hudc = 0x605000, proj_h = 0x606000, project = 0x607000,
                                root = 0x608000, comp_h = 0x609000, comp = 0x60A000, rp = 0x60B000,
                                table = 0x60C000, kids = 0x60D000, timer = 0x60E000, lap = 0x60F000,
                                position = 0x610000;
        memory.Put<std::uint64_t>(racer + 0x20, kart);
        memory.Put<std::uint64_t>(kart + 0x28, entity);
        memory.Put<std::uint64_t>(entity + 0x18, comps);
        memory.Put<std::uint32_t>(comps + 0x0C, 1);
        memory.Put<std::uint64_t>(comps + 0x20, items);
        memory.Put<std::uint64_t>(items, hudc);
        memory.Put<std::uint64_t>(hudc, host.main_base + 0x494D5A0);
        memory.Put<std::uint64_t>(hudc + 0x28, racer);
        memory.Put<std::uint64_t>(hudc + 0x38, proj_h);
        memory.Put<std::uint64_t>(proj_h + 0x28, project);
        memory.Put<std::uint64_t>(project + 0x10, root);
        node(root);
        memory.Put<std::uint64_t>(root + 0x38, comp_h);
        memory.Put<std::uint64_t>(comp_h + 0x28, comp);
        memory.Put<std::uint64_t>(comp + 0x20, rp);
        node(rp);
        memory.Put<std::uint64_t>(rp + 0x48, table);
        memory.Put<std::uint32_t>(table + 0x0C, 5);
        memory.Put<std::uint64_t>(table + 0x20, kids);
        for (const auto [i, at, y] : {std::tuple{0, timer, 138.0f}, {3, lap, 93.0f}, {4, position, 961.0f}}) {
            memory.Put<std::uint64_t>(kids + 8ull * i, at);
            node(at);
            memory.Put<float>(at + 0x64, y);
        }
        // the started race's leaders column: list -> item (in a loaded project) -> parent
        constexpr std::uint64_t leaders = 0x620000, list = 0x621000, array = 0x622000, item = 0x623000,
                                master = 0x624000;
        memory.Put<std::uint64_t>(leaders + 0x10, list);
        memory.Put<std::uint8_t>(leaders + 0x30, 1);
        memory.Put<std::uint32_t>(list + 0x0C, 1);
        memory.Put<std::uint64_t>(list + 0x20, array);
        memory.Put<std::uint64_t>(array, item);
        node(item);
        memory.Put<std::uint64_t>(item + 0x20, 1);
        memory.Put<std::uint64_t>(item + 0x40, master);
        node(master);
        // a running race clock
        constexpr std::uint64_t clock = 0x630000;
        memory.Put<std::uint64_t>(clock, host.main_base + 0x4955920);

        const auto race = [&](std::uint64_t id) {
            r.track = "t111_crash_cove";
            r.count = 1;
            r.race_id = id;
            r.player_address = racer;
            r.ScanForWidget(); // a new race: resets the scan state
            r.clock_component = clock;
            r.pass_leaders = {leaders};
        };
        const auto finish_pass = [&] {
            r.scan_cursor = r.scan_end; // the current pass is complete
            r.scan_focus = false;
            r.ResolveHudNodes();
            memory.page_probes = 0;
            for (int i = 0; i < 4; ++i)
                r.ScanForWidget();
            return memory.page_probes;
        };

        // shown minimap (option left on): behaviour unchanged, the scan keeps looking for it
        r.loading = true;
        r.TrackMinimapSuppression();
        r.loading = false;
        r.race_shown = true;
        race(0x1111);
        r.TrackMinimapSuppression();
        assert(r.MinimapExpected());
        assert(finish_pass() > 0 && !r.hud_found);

        // a race loaded with the option held off by us: no widget to wait for, the scan idles
        r.minimap_option_off = true;
        r.loading = true;
        r.race_shown = false;
        r.TrackMinimapSuppression();
        assert(!r.MinimapExpected()); // already while loading (the racers may change)
        r.loading = false;
        r.minimap_option_off = false; // restored once the clock ran: the HUD is built by then
        race(0x2222);
        r.race_shown = true;
        r.TrackMinimapSuppression();
        assert(r.map_suppressed_race == 0x2222 && !r.MinimapExpected());
        assert(finish_pass() == 0 && r.hud_found);
        assert(!r.hud_nodes.node[HudMap] && r.hud_nodes.node[HudLeaders] == master);
        assert(r.hud_nodes.node[HudTime] == timer && r.hud_nodes.node[HudLap] == lap &&
               r.hud_nodes.node[HudPosition] == position);

        // Restart (no loading screen, new racer objects): the HUD has its minimap again
        race(0x3333);
        r.TrackMinimapSuppression();
        assert(r.MinimapExpected() && !r.hud_found);
        assert(finish_pass() > 0 && !r.hud_found);
    }

    /// Published keys are cached per slot: they must still follow the racer in that slot.
    static void PublishedKeysFollowRacers() {
        static std::unordered_map<std::string, std::string> text;
        static std::unordered_map<std::string, std::int64_t> ints;
        text.clear();
        ints.clear();
        EdenDsmodHostApi host{};
        host.publish_i64 = [](void*, const char* n, std::int64_t v) { ints[n] = v; };
        host.publish_f64 = [](void*, const char*, double) {};
        host.publish_text = [](void*, const char* n, const char* v) { text[n] = v; };
        CtrArt::Library art;
        Reader r{art};
        r.host = &host;
        r.track = "t111_crash_cove";
        r.race_shown = true;
        r.count = 2;
        r.race_id = 1;
        r.racers[0] = {.address = 0x10, .position = 1, .driver = "DriverCrash", .display = "Crash",
                       .outfit = "DriverCrash_Outfit_Alt_01_materials"};
        r.racers[1] = {.address = 0x20, .ai = true, .position = 2, .driver = "DriverCoco", .display = ""};
        r.race_timed = true;
        r.race_secs = 75.25;
        r.Publish();
        assert(text["rc0.p"] == "module:ctr:portrait:DriverCrash:DriverCrash_Outfit_Alt_01_materials");
        assert(text["rc0.n"] == "module:ctr:name:n:CRASH");
        assert(text["rc1.p"] == "module:ctr:portrait:DriverCoco");
        assert(text["rc1.n"] == "module:ctr:name:w:COCO");
        // 75.25 s -> "1:15:25", right aligned in 8 slots
        assert(ints["tm.v"] == 1 && text["tm.c7"] == "module:ctr:tdigit:5" && text["tm.c5"] == "module:ctr:tdigit:colon" &&
               text["tm.c4"] == "module:ctr:tdigit:5" && text["tm.c1"] == "module:ctr:tdigit:1" && ints["tm.c0v"] == 0);
        // another skin, another name and the player's slot changing hands: keys rebuilt
        r.racers[0].outfit = "";
        r.racers[1].display = "Coco";
        r.racers[0].ai = true;
        r.racers[1].ai = false;
        r.Publish();
        assert(text["rc0.p"] == "module:ctr:portrait:DriverCrash" && text["rc0.n"] == "module:ctr:name:w:CRASH");
        assert(text["rc1.n"] == "module:ctr:name:n:COCO");
        r.race_timed = false;
        r.Publish();
        assert(ints["tm.v"] == 0);
    }

    static void Run() {
        Memory memory;
        EdenDsmodHostApi host{};
        host.userdata = &memory;
        host.main_base = 0x100000;
        host.is_mapped = Memory::Mapped;
        host.read_memory = Memory::Read;
        host.write_memory = Memory::Write;
        CtrArt::Library art;
        Reader r{art};
        r.host = &host;
        r.static_variant = 0;
        memory.Put<std::uint64_t>(host.main_base + 0x4D5D7F8, 0x200000);
        memory.Put<std::uint64_t>(0x200000 + 0xDB0, 0x210000);
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x220000);
        memory.Put<std::uint8_t>(0x220021, 1);
        r.loading = true;
        r.level_track = "t111_crash_cove";
        r.SteerMinimapOption();
        assert(r.minimap_option_off && memory.bytes.at(0x220021) == 0);
        memory.fail_write = true;
        r.RestoreMinimapOption();
        assert(r.minimap_option_off && memory.bytes.at(0x220021) == 0);
        memory.fail_write = false;
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x230000);
        memory.Put<std::uint8_t>(0x230021, 0);
        const int before = memory.writes;
        r.RestoreMinimapOption();
        assert(memory.writes == before && memory.bytes.at(0x230021) == 0);
        assert(!r.minimap_option_off); // verified replacement abandons old ownership
        r.SteerMinimapOption();
        assert(!r.minimap_option_off && memory.bytes.at(0x230021) == 0);
        // A replacement enabled option can be acquired in the same loading sample.
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x220000);
        memory.Put<std::uint8_t>(0x220021, 1);
        r.SteerMinimapOption();
        assert(r.minimap_option_off && memory.bytes.at(0x220021) == 0);
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x230000);
        memory.Put<std::uint8_t>(0x230021, 1);
        r.SteerMinimapOption();
        assert(r.minimap_option_off && r.minimap_option_address == 0x230021);
        assert(memory.bytes.at(0x230021) == 0 && memory.bytes.at(0x220021) == 0);
        // An unreadable replacement value is transient; preserve the pending owner.
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x240000);
        r.SteerMinimapOption();
        r.RestoreMinimapOption();
        assert(r.minimap_option_off && r.minimap_option_address == 0x230021);
        memory.Put<std::uint64_t>(0x210000 + 0x78, 0x230000);
        r.RestoreMinimapOption();
        assert(!r.minimap_option_off && memory.bytes.at(0x230021) == 1);
        // A player-disabled option is never acquired or changed.
        memory.Put<std::uint8_t>(0x230021, 0);
        r.SteerMinimapOption();
        assert(!r.minimap_option_off && memory.bytes.at(0x230021) == 0);
        // SHOW retries a failed HUD write instead of forgetting ownership.
        constexpr std::uint64_t node = 0x300000;
        memory.Put<std::uint64_t>(node, host.main_base + 0x47EFF68);
        memory.Put<std::uint8_t>(node + 0x0D, 0x04);
        r.hud_nodes.node[HudMap] = r.hud_ours[HudMap] = node;
        r.SetHudHidden(HudMap, false);
        memory.fail_write = true;
        r.ApplyHud();
        assert(r.hud_ours[HudMap] == node);
        memory.fail_write = false;
        r.ApplyHud();
        assert(!r.hud_ours[HudMap] && !(memory.bytes.at(node + 0x0D) & 0x04));
        // A replaced node whose give-back write fails stays pending and is retried.
        {
            constexpr std::uint64_t old_node = 0x310000, new_node = 0x320000, widget = 0x330000,
                                    sprite = 0x340000;
            const std::uint64_t placeable = host.main_base + 0x47EFF68;
            memory.Put<std::uint64_t>(old_node, placeable);
            memory.Put<std::uint8_t>(old_node + 0x0D, 0x04);
            memory.Put<std::uint64_t>(new_node, placeable);
            memory.Put<std::uint8_t>(new_node + 0x0D, 0x00);
            memory.Put<std::uint64_t>(sprite, placeable);
            memory.Put<std::uint64_t>(sprite + 0x40, new_node);
            memory.Put<std::uint64_t>(widget + 0x18, sprite);
            r.SetHudHidden(HudMap, true);
            r.hud_nodes = {};
            r.hud_nodes.node[HudMap] = r.hud_ours[HudMap] = old_node;
            r.widget_addr = widget;
            memory.fail_write = true;
            r.ResolveHudNodes();
            assert(r.hud_nodes.node[HudMap] == new_node && !r.hud_ours[HudMap]);
            assert(r.hud_release[HudMap] == old_node && (memory.bytes.at(old_node + 0x0D) & 0x04));
            r.ApplyHud(); // still failing: kept pending
            assert(r.hud_release[HudMap] == old_node);
            memory.fail_write = false;
            r.ApplyHud();
            assert(!r.hud_release[HudMap] && !(memory.bytes.at(old_node + 0x0D) & 0x04));
            assert(r.hud_ours[HudMap] == new_node && (memory.bytes.at(new_node + 0x0D) & 0x04));
            // a pending node that becomes the pick again is owned again (SHOW can undo it)
            memory.Put<std::uint8_t>(old_node + 0x0D, 0x04);
            r.hud_ours[HudMap] = 0;
            r.hud_release[HudMap] = old_node;
            r.hud_nodes.node[HudMap] = old_node;
            r.SetHudHidden(HudMap, false);
            r.ApplyHud();
            assert(!r.hud_release[HudMap] && !r.hud_ours[HudMap] && !(memory.bytes.at(old_node + 0x0D) & 0x04));
            r.widget_addr = 0;
            r.hud_nodes = {};
        }
        // A page-edge string must not accept a terminator beyond the caller's limit.
        for (std::uint64_t at = 0x400FF9; at < 0x401020; ++at)
            memory.Put<std::uint8_t>(at, at == 0x401009 ? 0 : 'a');
        assert(r.String(0x400FF9, 10).empty());
        assert(r.String(0x400FF9, 32) == std::string(16, 'a'));
    }
};
} // namespace CtrReader

int main() {
    CtrReader::ReaderTestAccess::Run();
    CtrReader::ReaderTestAccess::ScanIdlesWithoutMinimap();
    CtrReader::ReaderTestAccess::PublishedKeysFollowRacers();
}
