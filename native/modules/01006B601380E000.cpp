// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <thread>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <condition_variable>
#include <map>
#include <set>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include "kirby_art.h"
#include "kirby_assets.h"
#include "kirby_files.h"
#include "kirby_font.h"
#include "kirby_fx.h"
#include "kirby_layout.h"
#include "kirby_modes.h"
#include "kirby_soul.h"
#include "kirby_chara.h"
#include "kirby_colour.h"
#include "kirby_planet.h"
#include "kirby_gates.h"
#include "kirby_mago.h"

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x01006B601380E000);
// 1.0.0, the only release.
constexpr std::string_view BuildId =
    "FA24EA3C3A512C072A2062556C9C9692B3E6ACE1000000000000000000000000";
constexpr std::string_view KeyPrefix{"module:kirby:"};
constexpr std::size_t MaxRomfsFile = 64 * 1024 * 1024;

KirbyAssets::RomfsReader MakeRomfsReader(const EdenDsmodHostApi& h) {
    return [read_romfs = h.read_romfs,
            userdata = h.userdata](const std::string& path) -> std::optional<KirbyAssets::Bytes> {
        if (!read_romfs)
            return std::nullopt;
        const auto size = read_romfs(userdata, path.c_str(), 0, nullptr, 0);
        if (!size || size > MaxRomfsFile)
            return std::nullopt;
        KirbyAssets::Bytes out(size);
        if (read_romfs(userdata, path.c_str(), 0, out.data(), size) != size)
            return std::nullopt;
        return out;
    };
}

// Layout animation curves (BFLAN hermite keys: frame, value, slope per frame). Two keys on one frame
// are a step: the first ends the segment before it, the second starts the one after.
struct Key {
    float frame, value, slope;
};

float Curve(std::span<const Key> keys, float t) {
    if (t <= keys.front().frame)
        return keys.front().value;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const Key& a = keys[i];
        const Key& b = keys[i + 1];
        if (t >= b.frame || b.frame <= a.frame)
            continue;
        const float d = b.frame - a.frame, u = (t - a.frame) / d, u2 = u * u, u3 = u2 * u;
        return a.value * (2 * u3 - 3 * u2 + 1) + b.value * (3 * u2 - 2 * u3) + a.slope * d * (u3 - 2 * u2 + u) +
               b.slope * d * (u3 - u2);
    }
    return keys.back().value;
}

// TitleMenu: MainTitle_Push (W_StartBase), FileSelectBtn_Focus / _Unfocus / _Push (N_Push, P_Frash)
// and FileSelectBtnCursor_Focus / _Wait / _Unfocus / _Push (N_Top).
constexpr Key StartPush[]{{0, 1, -0.175f}, {2, 0.65f, 0}, {10, 1, 0.04375f}};
constexpr Key BtnFocus[]{{0, 0.92f, 0.130203f}, {2, 1.02f, 0}, {6, 1, 0}};
constexpr Key BtnUnfocus[]{{0, 1, 0}, {4, 0.92f, 0}};
constexpr Key BtnPush[]{{0, 1, -0.0700959f}, {3, 0.88f, -0.00144446f}, {3, 0.88f, 0.0465477f}, {8, 1, 0}};
constexpr Key BtnFlash[]{{0, 0, 31.8065f}, {3, 50, 1.41748f}, {3, 50, -17.8591f}, {8, 0, 0}};
constexpr Key CursorFocus[]{{0, 0, 101.687f}, {6, 255, 0}};
constexpr Key CursorWait[]{{0, 255, 0}, {30, 0, -15.6028f}, {30, 0, 12.5199f}, {60, 255, 0}};
constexpr Key CursorUnfocus[]{{0, 255, -150.21f}, {4, 0, 0}};
constexpr Key CursorPushX[]{{3, 1, 0.0114286f}, {10, 1.08f, 0.0114286f}};
constexpr Key CursorPushY[]{{3, 1, 0.0314286f}, {10, 1.22f, 0.0314286f}};
constexpr Key CursorPushAlpha[]{{3, 255, 0}, {10, 0, -58.2121f}};

struct Module {
    explicit Module(const EdenDsmodHostApi& host) : romfs{MakeRomfsReader(host)}, logger{host} {}
    EdenDsmodHostApi logger; // research: the pictures' drawing times, logged from the workers

    // read_romfs and its userdata outlive the instance, so this reader is safe on any thread.
    KirbyAssets::RomfsReader romfs;
    KirbyAssets::Decoder assets;
    // "Use souvenir" tap: open the enforce gate for one tick (the runtime presses X), then top up
    // the game's X-hold counter so the reserve item comes out within a couple of frames.
    std::atomic<bool> use_requested{false};
    // Buttons for the game: the manifest's enforce rules press one button each while the gate
    // kirby.press is open, each enabled by its own kirby.want_<button>; one press per tick.
    std::string press_next;
    void Press(std::string button) {
        press_next = std::move(button);
    }
    // The Options tab's settings live in persisted runtime flags, which a module cannot read: the
    // manifest's enforce rules hand each one's value to on_action ("opt", setting * 2 + value) while the
    // gate is open (for every press, a few times after the package loads, and when a flag is stored,
    // which its rule does first, on opt.set.<n>.<value>).
    static constexpr int Options = 139, SyncTicks = 120, SyncCount = 8, HealSeen = 5, BlastSeen = 8, EpiHud = 11, ColourBit = 13,
                         PinBit = 34, PinSlots = 5, PinBits = 7, StoreBlock = 7, StoreBlocks = (Options + StoreBlock) / StoreBlock;
    // 0 still background, then hide spheres, lives, stars, souvenir; the Heal and the Magolor Blast popups seen, per
    // file (HealSeen + file, BlastSeen + file); the Epilogue's hidden magic points and gauge (EpiHud, EpiHud + 1); Kirby's
    // colour per file, a preset's index in seven bits (ColourBit + 7 * file ..); the Magoland missions pinned per file,
    // five slots of a save index + 1 in seven bits (PinBit + 35 * file + 7 * slot ..)
    std::array<int, Options> options{};
    bool options_known{}; // handed over at least once
    int sync_tick{};
    // The flags to store on the next enforce tick, each value + 1 (set, not toggled: a rule may run twice while the gate
    // is open). Their rules sit in blocks of seven flags (a colour or a pin slot each, the first block one short), each
    // block behind a runtime flag of its own, so a press runs a dozen rules: the runtime spends a tick on every rule it
    // refuses, and a long tick is caught up at once, which ends the press before the game has seen it.
    std::array<std::int8_t, Options> store{};
    void Store(int i, int v) {
        options[static_cast<std::size_t>(i)] = v, store[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(v + 1);
    }
    void PublishPress(const EdenDsmodHostApi& host) {
        const bool sync = sync_tick < SyncTicks * SyncCount && ++sync_tick % SyncTicks == 0;
        if (colour_storing)
            for (int b = 0; b < ColourBits; ++b) {
                const auto i = static_cast<std::size_t>(ColourBit + ColourBits * colour_file + b);
                if (const int want = (colour_want >> b) & 1; options[i] != want)
                    store[i] = static_cast<std::int8_t>(want + 1);
            }
        std::array<bool, StoreBlocks> blocks{};
        for (std::size_t i = 0; i < store.size(); ++i)
            blocks[(i + 1) / StoreBlock] |= store[i] != 0;
        const bool storing = std::ranges::find(blocks, true) != blocks.end();
        host.publish_i64(host.userdata, "kirby.press", press_next.empty() && !sync && !storing ? 0 : 1);
        host.publish_i64(host.userdata, "store.on", storing ? 1 : 0);
        for (int k = 0; k < StoreBlocks; ++k) {
            host.publish_i64(host.userdata, ("store.blk." + std::to_string(k)).c_str(), blocks[static_cast<std::size_t>(k)] ? 1 : 0);
            if (blocks[static_cast<std::size_t>(k)])
                for (int i = std::max(k * StoreBlock - 1, 0); i < std::min((k + 1) * StoreBlock - 1, Options); ++i)
                    for (int v = 0; v < 2; ++v)
                        host.publish_i64(host.userdata, ("opt.set." + std::to_string(i) + "." + std::to_string(v)).c_str(),
                                         store[static_cast<std::size_t>(i)] == v + 1 ? 1 : 0);
        }
        store.fill(0);
        for (const char* b : {"x", "a", "up", "down", "map_up", "map_down", "map_left", "map_right", "plus", "minus", "y", "upx"})
            host.publish_i64(host.userdata, (std::string{"kirby.want_"} + b).c_str(), press_next == b ? 1 : 0);
        press_next.clear();
    }
    std::atomic<bool> title_tap{false};
    std::atomic<int> wmap_tap{-1};
    std::atomic<int> play_tap{-1};
    std::atomic<bool> resume_tap{false};
    std::atomic<int> epi_tap{-1};
    std::atomic<bool> epi_resume_tap{false};
    std::atomic<bool> epi_pop_tap{false};
    std::atomic<int> file_tap{-1};
    int use_frames{};
    std::uint32_t bg_frame{};
    std::atomic<bool> scan_requested{false};
    std::atomic<bool> ptrscan_requested{false};
    std::atomic<bool> statescan_requested{false};
    std::atomic<bool> bytefind_requested{false};
    std::atomic<bool> findseq_requested{false}, basefind_requested{false};
    std::atomic<bool> vscan_requested{false};
    std::atomic<bool> vtscan_requested{false};
    std::atomic<bool> layoutscan_requested{false};
    std::atomic<bool> trace_toggle{false};
    std::atomic<bool> render_requested{false};
    std::atomic<bool> fxwatch_toggle{false};
    std::atomic<bool> objfind_requested{false};
    std::atomic<bool> panewatch_requested{false};
    std::atomic<bool> bgmwatch_toggle{false};
    // Research: put the Ability Star souvenir in the reserve slot and use it at once; starhunt also
    // narrows the heap to the words that held the ability it gives while the star was out
    std::atomic<bool> star_requested{false}, hunt_requested{false};
    std::atomic<int> newhunt_requested{0};
    int star_wait{}, hunt_phase{}, hunt_ticks{}, hunt_delay{};
    std::chrono::steady_clock::time_point hunt_at;
    // every 8-aligned word of the heap holding a vtable pointer (main's vtable range), by address
    std::unordered_map<u64, u64> before;
    struct Fresh {
        u64 at, vt;
        std::vector<u8> bytes;
    };
    std::vector<Fresh> fresh;
    u64 force_vt{}, force_off{};
    std::unordered_map<u64, u64> VtablesNow(const EdenDsmodHostApi& host) const {
        std::unordered_map<u64, u64> out;
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        const u64 vlo = host.main_base + 0x1500000, vhi = host.main_base + 0x1640000;
        for (u64 page = lo & ~u64{0xFFF}; page < hi; page += 0x1000) {
            const u8* q = host.get_read_pointer(host.userdata, page, 0x1000);
            if (!q)
                continue;
            for (u32 o = 0; o < 0x1000; o += 8) {
                u64 v;
                std::memcpy(&v, q + o, 8);
                if (v >= vlo && v < vhi)
                    out.emplace(page + o, v);
            }
        }
        return out;
    }
    bool hunt_posed{};

    void HuntStep(const EdenDsmodHostApi& host) {
        u64 root{}, rec{}, p{}, comp{};
        u32 held{}, ability{};
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (!rd(host.main_base + 0x16325B8, root) || !rd(root + 0x260, rec) || !rd(rec + 0x2A4, held))
            return;
        if (rd(rec + 0x140, p) && rd(p + 0x220, comp))
            rd(comp + 0x30, ability);
        ++hunt_ticks;
        if (hunt_phase == 3) {
            // newhunt / newforce: once the pose is over, the objects that appeared since the start (a vtable
            // pointer where there was none) are the star's candidates
            hunt_posed |= held == 1;
            if (hunt_posed && held == 0 && ++hunt_wait > 15) {
                fresh.clear();
                for (const auto& [at, vt] : VtablesNow(host))
                    if (const auto it = before.find(at); it == before.end() || it->second != vt) {
                        Fresh f{at, vt, {}};
                        f.bytes.resize(0x400);
                        if (dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, f.bytes.data(), f.bytes.size()))
                            fresh.push_back(std::move(f));
                    }
                std::string line = "Kirby DSMod newhunt " + std::to_string(fresh.size()) + " new objects";
                if (force_vt) {
                    // newforce: the chosen ability into the matching new objects' field
                    int n = 0;
                    for (const auto& f : fresh)
                        if (f.vt == force_vt) {
                            const u32 want = static_cast<u32>(CheatAbilities[cheat_index]);
                            host.write_memory(host.userdata, f.at + force_off, &want, 4);
                            ++n;
                        }
                    line += ", forced " + std::to_string(n);
                }
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
                hunt_phase = 4;
            }
        } else if (hunt_phase == 4 && ability != 0 && ability < 40) {
            // which new objects held the ability given, at which offset (class main+X, offset)
            std::map<std::string, int> hits;
            for (const auto& f : fresh)
                for (std::size_t o = 0; o + 4 <= f.bytes.size(); o += 4) {
                    u32 v;
                    std::memcpy(&v, f.bytes.data() + o, 4);
                    if (v == ability) {
                        char key[48];
                        std::snprintf(key, sizeof key, " main+%llx+%zx", static_cast<unsigned long long>(f.vt - host.main_base), o);
                        ++hits[key];
                    }
                }
            std::string line = "Kirby DSMod newhunt got " + std::to_string(ability) + ":";
            for (const auto& [key, n] : hits)
                line += key + (n > 1 ? "x" + std::to_string(n) : "");
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.substr(0, 4000).c_str());
            hunt_phase = 0;
        } else if (hunt_phase == 1) {
            hunt_posed |= held == 1;
            if (hunt_posed && held == 0 && ++hunt_wait > 15) {
                HeapOp(host, "changed", 0, "starhunt changed");
                HeapOp(host, "mark", 0, "starhunt mark");
                hunt_phase = 2;
            }
        } else if (hunt_phase == 2 && ability != 0 && ability < 40) {
            HeapOp(host, "beq", ability, "starhunt beq " + std::to_string(ability));
            // each survivor's object: the nearest vtable pointer before it (main+X) and the field's offset
            std::map<std::string, int> owners;
            for (const auto& h : hscan)
                for (int i = 0; i < 1024; ++i) {
                    if (h.cur[i] == 255)
                        continue;
                    const u64 at = h.page + static_cast<u64>(i) * 4;
                    for (u64 back = 0; back <= 0x800; back += 8) {
                        u64 v{};
                        if (!rd((at & ~u64{7}) - back, v))
                            break;
                        if (v >= host.main_base + 0x1500000 && v < host.main_base + 0x1640000) {
                            char key[64];
                            std::snprintf(key, sizeof key, " main+%llx+%llx", static_cast<unsigned long long>(v - host.main_base),
                                          static_cast<unsigned long long>(at - ((at & ~u64{7}) - back)));
                            ++owners[key];
                            break;
                        }
                    }
                }
            std::string line = "Kirby DSMod starhunt owners " + std::to_string(ability) + ":";
            for (const auto& [key, n] : owners)
                line += key + (n > 1 ? "x" + std::to_string(n) : "");
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.substr(0, 4000).c_str());
            hunt_phase = 0;
        }
        if (hunt_ticks > 900)
            hunt_phase = 0;
    }
    int hunt_wait{};

    // Research (Magolor's last blows, Magolor Soul's attacks): a second after each room change, and half a second after
    // the Arenas' boss panels change names, the heap is scanned for the hero script contexts, the other script contexts
    // (vtable main+0x15BEA80, the anim controller's pointer at +0x1540: the bosses') and Magolor phase 1's custom object
    // (vtable main+0x15B4BA0, barrier u32 +0x1C); each tick logs the page, the room and the boss flags when they change,
    // every context's kind change, its frame every tick while a hero kind from 0x280 to 0x2C0 runs, the barrier, the
    // boss panels' names and lives and the music's sound id
    bool fxwatch{};
    std::vector<u64> fxw_heroes, fxw_bosses, fxw_scripts;
    u64 fxw_room{}, fxw_tick{};
    int fxw_wait{};
    std::string fxw_last, fxw_panels;

    void FxWatch(const EdenDsmodHostApi& host) {
        ++fxw_tick;
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 room = rd(rd(rd(host.main_base + 0x16325B8) + 0x260) + 0x140);
        if (room != fxw_room)
            fxw_room = room, fxw_wait = 60, fxw_heroes.clear(), fxw_bosses.clear(), fxw_scripts.clear();
        std::string panels, names;
        for (const auto& p : ArenaPanels(host)) {
            char life[16];
            std::snprintf(life, sizeof life, " %.0f]", p.life * 100);
            names += " [" + std::string(p.name.begin(), p.name.end());
            panels += " [" + std::string(p.name.begin(), p.name.end()) + life;
        }
        if (names != fxw_panels)
            fxw_panels = names, fxw_wait = std::max(fxw_wait, 30);
        if (fxw_wait > 0 && --fxw_wait == 0) {
            fxw_heroes.clear(), fxw_bosses.clear(), fxw_scripts.clear();
            const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
            const u64 hero = host.main_base + HeroContextVtable, boss = host.main_base + 0x15B4BA0, script = host.main_base + 0x15BEA80;
            for (u64 page = lo & ~u64{0xFFF}; page < hi; page += 0x1000)
                if (const u8* q = host.get_read_pointer(host.userdata, page, 0x1000))
                    for (u32 at = 0; at < 0x1000; at += 8) {
                        u64 v;
                        std::memcpy(&v, q + at, 8);
                        if (v == hero)
                            fxw_heroes.push_back(page + at);
                        else if (v == boss)
                            fxw_bosses.push_back(page + at);
                        else if (v == script)
                            fxw_scripts.push_back(page + at);
                    }
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod fxwatch scan: " + std::to_string(fxw_heroes.size()) + " hero(es), " +
                                                          std::to_string(fxw_scripts.size()) + " script(s), " +
                                                          std::to_string(fxw_bosses.size()) + " Magolor").c_str());
        }
        const char* page = host.get_text ? host.get_text(host.userdata, "__page") : nullptr;
        char buf[400];
        const u64 scene = rd(host.main_base + 0x1642EE8), scene_vt = rd(scene);
        u32 movie{};
        dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x1626C30, &movie, 4);
        const auto value = [&](const char* n) { return host.get_i64 ? host.get_i64(host.userdata, n, -9) : -9; };
        const u64 slot_vt = rd(rd(rd(host.main_base + 0x16325B8) + 0xB0)), state_vt = room ? rd(rd(room + 0x6C0) + 0xA0) : 0;
        const u64 menu_words = rd(rd(host.main_base + 0x16325B8) + 0x378);
        std::snprintf(buf, sizeof buf, "page=%s room=%llx boss=%d world=%d on=%d hud=%d posing=%d scene=main+%llx movie=%u screen=%lld film=%lld game=%d wmap=%d slot=%llx state=%llx menu=%llx",
                      page ? page : "-", static_cast<unsigned long long>(room), play_boss, play_world, play_on ? 1 : 0, play_hud ? 1 : 0,
                      play_posing ? 1 : 0, static_cast<unsigned long long>(scene_vt ? scene_vt - host.main_base : 0), movie,
                      static_cast<long long>(value("title.screen")), static_cast<long long>(value("play.movie")), from_game ? 1 : 0,
                      wmap_live ? 1 : 0, static_cast<unsigned long long>(slot_vt ? slot_vt - host.main_base : 0),
                      static_cast<unsigned long long>(state_vt ? state_vt - host.main_base : 0), static_cast<unsigned long long>(menu_words));
        std::string key = buf, frames;
        bool finishing = false;
        for (std::size_t i = 0; i < fxw_heroes.size(); ++i) {
            const u64 h = fxw_heroes[i];
            if (rd(h) != host.main_base + HeroContextVtable)
                continue;
            const u64 ctl = h + 0x12D8, z = rd(rd(rd(ctl) + 0x848) + 0x70);
            const u32 kind = static_cast<u32>(rd(ctl + 0x38));
            float v[3]{};
            if (z)
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, z + 0x48, v, 12);
            std::snprintf(buf, sizeof buf, " h%zu=%x/%.0f", i, kind, v[0]);
            key += buf;
            std::snprintf(buf, sizeof buf, " f%zu=%.1f@%.2f", i, v[1], v[2]);
            frames += buf;
            finishing |= kind >= 0x280 && kind < 0x2C0;
        }
        for (std::size_t i = 0; i < fxw_bosses.size(); ++i)
            if (rd(fxw_bosses[i]) == host.main_base + 0x15B4BA0) {
                std::snprintf(buf, sizeof buf, " b%zu=%u", i, static_cast<u32>(rd(fxw_bosses[i] + 0x1C)));
                key += buf;
            }
        for (std::size_t i = 0; i < fxw_scripts.size(); ++i) {
            const u64 c = fxw_scripts[i], ctl = rd(c + 0x1540);
            if (rd(c) != host.main_base + 0x15BEA80 || !ctl)
                continue;
            const u64 z = rd(rd(rd(ctl) + 0x848) + 0x70);
            float v[3]{};
            if (z)
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, z + 0x48, v, 12);
            std::snprintf(buf, sizeof buf, " s%zu@%llx=%x/%.0f", i, static_cast<unsigned long long>(c), static_cast<u32>(rd(ctl + 0x38)), v[0]);
            key += buf;
            std::snprintf(buf, sizeof buf, " g%zu=%.1f@%.2f", i, v[1], v[2]);
            frames += buf;
        }
        const u64 player = rd(rd(rd(rd(rd(host.main_base + 0x16324C8) + 8) + 8) + 0xAC8) + 0x34B40 + 0x2DE00);
        const u64 handle = player ? rd(player + (static_cast<u32>(rd(player + 0x170)) == 0 ? 0x10 : 0x18)) : 0;
        std::snprintf(buf, sizeof buf, " epig=%d live=%d bgm=%x", epi_gauge, epi_live ? 1 : 0, handle ? static_cast<u32>(rd(handle + 0x184)) : 0);
        key += buf + panels;
        // every 6 ticks too, to follow the animation frames between changes
        if (key == fxw_last && !finishing && fxw_tick % 6)
            return;
        fxw_last = key;
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod fxwatch t=" + std::to_string(fxw_tick) + " " + key + frames).c_str());
    }

    // Research: the objects with a vtable (user:objfind.txt, its main offset in hex) anywhere in the heap
    void ObjFind(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:objfind.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod objfind: no user:objfind.txt");
        const u64 vt = host.main_base + std::strtoull(std::string(text->begin(), text->end()).c_str(), nullptr, 16);
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        char buf[64];
        std::snprintf(buf, sizeof buf, "Kirby DSMod objfind main=%llx:", static_cast<unsigned long long>(host.main_base));
        std::string msg = buf;
        int found = 0;
        for (u64 page = lo & ~u64{0xFFF}; page < hi && found < 32; page += 0x1000)
            if (const u8* q = host.get_read_pointer(host.userdata, page, 0x1000))
                for (u32 at = 0; at < 0x1000; at += 8) {
                    u64 v;
                    std::memcpy(&v, q + at, 8);
                    if (v == vt)
                        std::snprintf(buf, sizeof buf, " %llx", static_cast<unsigned long long>(page + at)), msg += buf, ++found;
                }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    // Research: the music player, App.Application's sound manager [[[[main+0x16324C8]+8]+8]+0xAC8]+0x34B40, its BgmPlayer
    // at +0x2DE00 (get_PlayFrameCount: [player+0x170] picks the handle at +0x10 or +0x18, its +0x180): logged every 30
    // ticks with the sound ids (0x1000000..0x10003FF) found in the player, the handle and what their words point to
    bool bgmwatch{};
    int bgm_tick{};
    void BgmWatch(const EdenDsmodHostApi& host) {
        if (++bgm_tick % 30)
            return;
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 snd = rd(rd(rd(rd(host.main_base + 0x16324C8) + 8) + 8) + 0xAC8), manager = snd ? snd + 0x34B40 : 0;
        const u64 player = rd(manager + 0x2DE00);
        const u32 which = static_cast<u32>(rd(player + 0x170));
        const u64 handle = rd(player + (which == 0 ? 0x10 : 0x18));
        char buf[96];
        std::snprintf(buf, sizeof buf, "Kirby DSMod bgmwatch t=%d player=%llx which=%u handle=%llx frame=%u:", bgm_tick, static_cast<unsigned long long>(player),
                      which, static_cast<unsigned long long>(handle), static_cast<u32>(rd(handle + 0x180)));
        std::string msg = buf;
        const auto ids = [&](const char* tag, u64 at, u32 size, bool deep) {
            std::vector<u8> d(size);
            if (!at || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, d.data(), size))
                return;
            for (u32 o = 0; o + 4 <= size; o += 4) {
                u32 v;
                std::memcpy(&v, d.data() + o, 4);
                if (v >= 0x1000000 && v < 0x1000400)
                    std::snprintf(buf, sizeof buf, " %s+%x=%x", tag, o, v), msg += buf;
            }
            if (!deep)
                return;
            for (u32 o = 0; o + 8 <= size; o += 8) {
                u64 p;
                std::memcpy(&p, d.data() + o, 8);
                if (p < 0x1000000 || p == at || (p & 7))
                    continue;
                std::vector<u8> e(0x200);
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p, e.data(), 0x200))
                    continue;
                for (u32 q = 0; q + 4 <= 0x200; q += 4) {
                    u32 v;
                    std::memcpy(&v, e.data() + q, 4);
                    if (v >= 0x1000000 && v < 0x1000400)
                        std::snprintf(buf, sizeof buf, " %s+%x>%x=%x", tag, o, q, v), msg += buf;
                }
            }
        };
        ids("p", player, 0x400, true);
        ids("h", handle, 0x400, true);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    // Research: the layout panes named in user:panewatch.txt (the name at the pane's +0xB0, found anywhere in the heap);
    // the first 64 words of each are then logged as floats every 30 ticks for 20 times, to see what moves
    std::vector<u64> pw_panes;
    int pw_left{}, pw_tick{};
    void PaneFind(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:panewatch.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod panewatch: no user:panewatch.txt");
        std::string name(text->begin(), text->end());
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
        pw_panes.clear();
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        for (u64 page = lo & ~u64{0xFFF}; page < hi && pw_panes.size() < 16; page += 0x1000)
            if (const u8* q = host.get_read_pointer(host.userdata, page, 0x1000))
                for (u32 at = 0; at + name.size() + 1 <= 0x1000; at += 8)
                    if (std::memcmp(q + at, name.data(), name.size()) == 0 && q[at + name.size()] == 0 && page + at >= lo + 0xB0)
                        pw_panes.push_back(page + at - 0xB0);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod panewatch: " + std::to_string(pw_panes.size()) + " pane(s) " + name).c_str());
        pw_left = 20, pw_tick = 0;
    }
    void PaneWatch(const EdenDsmodHostApi& host) {
        if (pw_left <= 0 || ++pw_tick % 30)
            return;
        --pw_left;
        for (const u64 pane : pw_panes) {
            float v[64]{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane, v, sizeof v))
                continue;
            char buf[32];
            std::snprintf(buf, sizeof buf, "Kirby DSMod panewatch %llx:", static_cast<unsigned long long>(pane));
            std::string msg = buf;
            for (int i = 0; i < 64; ++i)
                std::snprintf(buf, sizeof buf, " %d=%g", i * 4, v[i]), msg += buf;
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
        }
    }

    // Research: draws the gen/files pictures named in user:render.txt (a key a line) into render/<key>.rgba (u32 w, u32 h,
    // RGBA) under the emulator's folder, for offline previews drawn by the module's own code
    void RenderDump(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:render.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod render: no user:render.txt");
        std::filesystem::create_directories("render");
        const std::string list(text->begin(), text->end());
        int drawn = 0;
        for (std::size_t a = 0, b; a < list.size(); a = b + 1) {
            b = std::min(list.find('\n', a), list.size());
            std::string key = list.substr(a, b - a);
            while (!key.empty() && (key.back() == '\r' || key.back() == ' '))
                key.pop_back();
            if (key.empty())
                continue;
            const auto image = RenderFiles(key);
            if (!image) {
                host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, ("Kirby DSMod render: cannot draw " + key).c_str());
                continue;
            }
            std::replace(key.begin(), key.end(), '/', '_');
            std::ofstream out("render/" + key + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(&image->width), 4).write(reinterpret_cast<const char*>(&image->height), 4);
            out.write(reinterpret_cast<const char*>(image->rgba.data()), static_cast<std::streamsize>(image->rgba.size()));
            ++drawn;
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod render: " + std::to_string(drawn) + " drawn").c_str());
    }

    // Research: per-frame watch of the root object, Kirby's record, Kirby, his copy-ability component
    // and the running scene. Words that change during the first TraceLearn frames spent in a stage are
    // noise; after that every change of a quiet word is logged with the page state. The state line
    // (pages, loading, menu) is logged whenever it changes, from the start.
    struct TraceRegion {
        const char* name;
        u64 base{};
        std::vector<u32> last;
        std::vector<u16> changes;
    };
    static constexpr int TraceLearn = 900;
    bool tracing{};
    int trace_learnt{};
    u64 trace_tick{};
    std::string trace_state;
    std::array<TraceRegion, 5> trace{{{"root"}, {"rec"}, {"P"}, {"C"}, {"scene"}}};

    void TraceStep(const EdenDsmodHostApi& host) {
        ++trace_tick;
        const auto rd = [&](u64 at) {
            u64 v{};
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 root = rd(host.main_base + 0x16325B8), rec = root ? rd(root + 0x260) : 0, p = rec ? rd(rec + 0x140) : 0;
        const u64 bases[5]{root, rec, p, p ? rd(p + 0x220) : 0, rd(host.main_base + 0x1642EE8)};
        static constexpr std::size_t sizes[5]{0x1D000, 0x1000, 0x2000, 0x200, 0x1000};
        if (bases[2] != trace[2].base || bases[3] != trace[3].base || bases[4] != trace[4].base) {
            char line[160];
            std::snprintf(line, sizeof line, "Kirby DSMod trace bases root=%llx rec=%llx P=%llx C=%llx scene=%llx",
                          static_cast<unsigned long long>(bases[0]), static_cast<unsigned long long>(bases[1]),
                          static_cast<unsigned long long>(bases[2]), static_cast<unsigned long long>(bases[3]),
                          static_cast<unsigned long long>(bases[4]));
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        char head[160];
        std::snprintf(head, sizeof head, "map=%d play=%d stage=%d hud=%d posing=%d busy=%d set=%d", wmap_live ? 1 : 0,
                      play_on ? 1 : 0, play_stage ? 1 : 0, play_hud ? 1 : 0, play_posing ? 1 : 0, play_busy >= 4 ? 1 : 0, wmap_set);
        const std::string state = std::string{head} + " " + play_souv_state + " page=" + wmap_page[wmap_set];
        if (state != trace_state) {
            trace_state = state;
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod trace " + std::to_string(trace_tick) + " " + state).c_str());
        }
        const bool learning = trace_learnt < TraceLearn;
        if (learning && play_stage && play_on)
            ++trace_learnt;
        std::string line;
        int logged = 0;
        for (int r = 0; r < 5; ++r) {
            auto& t = trace[r];
            const std::size_t words = sizes[r] / 4;
            std::vector<u32> now(words);
            bool ok = bases[r] != 0;
            for (std::size_t at = 0; ok && at < sizes[r]; at += 0x1000)
                ok = dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(
                    host, bases[r] + at, reinterpret_cast<u8*>(now.data()) + at, std::min<std::size_t>(0x1000, sizes[r] - at));
            if (!ok || bases[r] != t.base) {
                t.base = ok ? bases[r] : 0;
                t.last = ok ? std::move(now) : std::vector<u32>{};
                if (t.changes.size() != words)
                    t.changes.assign(words, 0);
                continue;
            }
            for (std::size_t i = 0; i < words; ++i) {
                if (now[i] == t.last[i])
                    continue;
                if (learning) {
                    if (play_stage && play_on)
                        t.changes[i] = 1000;
                } else if (t.changes[i] < 40 && ++t.changes[i] < 40 && logged < 40) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, " %s+%zX:%X>%X", t.name, i * 4, t.last[i], now[i]);
                    line += buf, ++logged;
                }
            }
            t.last = std::move(now);
        }
        if (!line.empty())
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod trace " + std::to_string(trace_tick) + line).c_str());
        if (trace_learnt == TraceLearn) {
            ++trace_learnt;
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, "Kirby DSMod trace learnt");
        }
    }

    // Research: every layout object the title menu points at, its first pane names and the
    // visibility / alpha of its N_Top, to tell its screens apart. user:layoutscan.txt "<pane> [root]"
    // lists the layouts holding that pane instead, searched from the scope or the root object.
    void LayoutScan(const EdenDsmodHostApi& host) {
        u64 root{}, scope{};
        if (!host.log || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x16325B8, &root, 8) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, root + 0x1930, &scope, 8))
            return;
        char want[48]{}, from[8]{}, panes[8]{}, only[32]{};
        if (const auto t = romfs("user:layoutscan.txt")) {
            const std::string line(t->begin(), t->end());
            std::sscanf(line.c_str(), "%47s %7s %7s %31s", want, from, panes, only);
        }
        if (std::string_view{from} == "root")
            scope = root;
        // objects one and two steps from the scope, then the layouts they point at
        std::vector<std::pair<u64, std::string>> owners{{scope, "S"}};
        std::unordered_set<u64> seen{scope};
        for (std::size_t k = 0; k < owners.size() && k < 6000; ++k) {
            const auto [obj, path] = owners[k];
            if (std::count(path.begin(), path.end(), '+') >= 3)
                continue;
            std::vector<u8> m(0x1000);
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, obj, m.data(), m.size()))
                continue;
            for (std::size_t o = 0; o + 8 <= m.size(); o += 8) {
                u64 p{};
                std::memcpy(&p, &m[o], 8);
                if (p >= 0x2000000000 && p < 0x2200000000 && seen.insert(p).second) {
                    char step[32];
                    std::snprintf(step, sizeof step, "+%zX", o);
                    owners.emplace_back(p, path + step);
                }
            }
        }
        for (const auto& [p, o] : owners) {
            u64 vt{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p, &vt, 8) || vt != host.main_base + 0x15A18D0)
                continue;
            std::vector<char> b(panes[0] ? 0xC000 : 0x6000);
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p, b.data(), b.size()))
                continue;
            std::string names, top, all;
            for (std::size_t at = 0xB0; at + 0x18 < b.size(); at += 8) {
                if ((b[at] != 'N' && b[at] != 'L' && b[at] != 'P') || b[at + 1] != '_')
                    continue;
                const std::string name{&b[at], strnlen(&b[at], 24)};
                if (names.size() < 160)
                    names += name + " ";
                all += name + " ";
                if (name == "N_Top" && top.empty())
                    top = "flags=" + std::to_string(static_cast<u8>(b[at - 0xB0 + 0x58])) +
                          " alpha=" + std::to_string(static_cast<u8>(b[at - 0xB0 + 0x59]));
            }
            if (want[0] ? all.find(want) == std::string::npos
                        : names.find("L_Btn") == std::string::npos && names.find("L_MenuFrame") == std::string::npos &&
                              names.find("W_Frame") == std::string::npos)
                continue;
            char line[512];
            std::snprintf(line, sizeof line, "Kirby DSMod layout %s %llX N_Top %s: %s", o.c_str(),
                          static_cast<unsigned long long>(p), top.c_str(), names.c_str());
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
            // "<pane> <scope> panes": every pane's offset, flags and alpha too
            if (std::string_view{panes} == "panes") {
                std::string list;
                for (std::size_t at = 0xB0; at + 0x18 < b.size(); at += 8)
                    if ((b[at] == 'N' || b[at] == 'L' || b[at] == 'P' || b[at] == 'T' || b[at] == 'W') && b[at + 1] == '_' &&
                        std::string_view{&b[at], strnlen(&b[at], 24)}.find(only) != std::string_view::npos) {
                        char item[64];
                        std::snprintf(item, sizeof item, " %s@%zX:%u/%u", std::string{&b[at], strnlen(&b[at], 24)}.c_str(),
                                      at - 0xB0, static_cast<u8>(b[at - 0xB0 + 0x58]), static_cast<u8>(b[at - 0xB0 + 0x59]));
                        list += item;
                        if (list.size() > 600) {
                            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod panes" + list).c_str());
                            list.clear();
                        }
                    }
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod panes" + list).c_str());
            }
        }
    }
    struct VtSlot {
        u64 at;   // main static holding a heap pointer
        s32 off;  // -1: the object itself, else the slot inside it that holds the object
        s32 off2; // -1, or a further slot inside that object
        u64 vt;   // vtable, main-relative
    };
    std::vector<VtSlot> vtscan;
    std::atomic<bool> hscan_requested{false};
    std::vector<std::pair<u64, u32>> vscan;
    bool vscan_float{};
    int vscan_track{}; // frames left in which survivors must move one way only and end moved
    bool vscan_track_down{};
    std::unordered_map<u64, u32> vscan_start;
    struct Pulse {
        u32 start, last;
        int rises;
        bool falling, dead;
    };
    std::unordered_map<u64, Pulse> vscan_pulse; // track ... pulse: rise from rest, then fall back
    std::unordered_map<u64, u32> last_scan;

    // Research: heap blocks shaped like the copy-ability component (owner/heap pointers at +0..+0x10,
    // ids at +0x30 and +0x74, pending flag at +0x70); logs the ones whose id changed since the
    // previous scan. Components are assumed not to straddle a page.
    void ScanPlayer(const EdenDsmodHostApi& host) {
        const u64 begin = host.get_heap_begin(host.userdata), end = host.get_heap_end(host.userdata);
        int hits = 0;
        std::unordered_map<u64, u32> scan;
        for (u64 page = begin; page < end; page += 0x1000) {
            const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
            if (!p)
                continue;
            for (u32 at = 0; at + 0x80 <= 0x1000; at += 8) {
                u64 c0, c8, c10;
                std::memcpy(&c0, p + at, 8);
                std::memcpy(&c8, p + at + 8, 8);
                std::memcpy(&c10, p + at + 0x10, 8);
                const auto heap = [&](u64 v) { return v >= begin && v < end && !(v & 7); };
                if (!heap(c0) || !heap(c8) || !heap(c10))
                    continue;
                u32 ability, pending;
                std::memcpy(&ability, p + at + 0x30, 4);
                std::memcpy(&pending, p + at + 0x74, 4);
                if (ability > 0x26 || pending > 0x26 || p[at + 0x70] > 1)
                    continue;
                const u64 comp = page + at;
                scan[comp] = ability;
                u64 back{};
                if (dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, c0 + 0x220, &back, 8) &&
                    back == comp) {
                    const std::string own = "Kirby DSMod scan owner" + fmt(c0, 0, comp, ability);
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, own.c_str());
                }
                if (const auto it = last_scan.find(comp); it != last_scan.end() && it->second != ability) {
                    const std::string hit = "Kirby DSMod scan hit" + fmt(c0, 0, comp, ability) +
                                            " was " + std::to_string(it->second);
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, hit.c_str());
                    ++hits;
                }
            }
        }
        const std::string line = "Kirby DSMod scan_player: " + std::to_string(scan.size()) +
                                 " object(s), " + std::to_string(hits) + " changed";
        last_scan = std::move(scan);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
    }

    // Research: live state objects. user:statescan.txt lists "<vptr main offset hex> <name>" per line (the
    // states' vtables); logs every heap object whose first word is one of them, with up to 6 heap words
    // pointing at it.
    void StateScan(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:statescan.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod statescan: no user:statescan.txt");
        std::unordered_map<u64, std::string> vt;
        const std::string all(text->begin(), text->end());
        for (std::size_t at = 0; at < all.size();) {
            const std::size_t eol = std::min(all.find('\n', at), all.size());
            unsigned long long off{};
            char name[96]{};
            if (std::sscanf(all.substr(at, eol - at).c_str(), "%llx %95s", &off, name) == 2)
                vt[host.main_base + off] = name;
            at = eol + 1;
        }
        const u64 begin = host.get_heap_begin(host.userdata), end = host.get_heap_end(host.userdata);
        std::unordered_map<u64, std::string> found;
        for (u64 page = begin; page < end; page += 0x1000) {
            const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
            if (!p)
                continue;
            for (u32 at = 0; at < 0x1000; at += 8) {
                u64 v;
                std::memcpy(&v, p + at, 8);
                if (const auto it = vt.find(v); it != vt.end())
                    found[page + at] = it->second;
            }
        }
        std::unordered_map<u64, std::vector<u64>> holders;
        for (u64 page = begin; page < end && !found.empty(); page += 0x1000) {
            const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
            if (!p)
                continue;
            for (u32 at = 0; at < 0x1000; at += 8) {
                u64 v;
                std::memcpy(&v, p + at, 8);
                if (const auto it = found.find(v); it != found.end() && holders[v].size() < 6)
                    holders[v].push_back(page + at);
            }
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod statescan: " + std::to_string(found.size()) + " state(s)").c_str());
        for (const auto& [obj, name] : found) {
            std::string msg = "Kirby DSMod state " + name + " @" + KirbyHex(obj) + " held by";
            for (const u64 h : holders[obj])
                msg += " " + KirbyHex(h);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
        }
    }

    // Research: every place in the heap and main holding the bytes of user:bytefind.txt (hex, spaces allowed).
    void ByteFind(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:bytefind.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod bytefind: no user:bytefind.txt");
        std::vector<u8> pat;
        int nib = -1;
        for (const char c : *text) {
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0)
                continue;
            if (nib < 0)
                nib = v;
            else
                pat.push_back(static_cast<u8>(nib * 16 + v)), nib = -1;
        }
        if (pat.empty() || pat.size() > 0x800)
            return;
        std::string msg = "Kirby DSMod bytefind " + std::to_string(pat.size()) + " byte(s):";
        int hits = 0;
        std::vector<u8> page(0x1000 + pat.size());
        const auto scan = [&](u64 begin, u64 end) {
            for (u64 at = begin; at < end && hits < 40; at += 0x1000) {
                const u8* p = host.get_read_pointer(host.userdata, at, 0x1000);
                if (!p)
                    continue;
                const u8* q = host.get_read_pointer(host.userdata, at + 0x1000, pat.size());
                std::memcpy(page.data(), p, 0x1000);
                if (q)
                    std::memcpy(page.data() + 0x1000, q, pat.size());
                else
                    std::fill(page.begin() + 0x1000, page.end(), 0);
                for (std::size_t i = 0; i < 0x1000 && hits < 40; ++i)
                    if (page[i] == pat[0] && std::memcmp(&page[i], pat.data(), pat.size()) == 0)
                        msg += " " + KirbyHex(at + i), ++hits;
            }
        };
        scan(host.get_heap_begin(host.userdata), host.get_heap_end(host.userdata));
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    // Research: multi-level pointer scan from the address in user:ptrscan.txt ("<hex> [depth]
    // [max_offset]") to holders in main's data/bss; logs every static chain found.
    void PointerScan(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:ptrscan.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod ptrscan: no user:ptrscan.txt");
        const std::string line(text->begin(), text->end());
        unsigned long long target{};
        int depth = 4;
        unsigned long long max_off = 0x800;
        std::sscanf(line.c_str(), "%llx %d %llx", &target, &depth, &max_off);
        const u64 heap_lo = host.get_heap_begin(host.userdata), heap_hi = host.get_heap_end(host.userdata);
        const u64 main_lo = host.main_base, main_hi = host.main_base + host.main_size;
        struct Link {
            u64 target;
            u64 offset;
        };
        std::vector<std::map<u64, Link>> levels; // per level: holder -> (target, offset)
        std::vector<u64> targets{target};
        int statics = 0;
        const auto scan_range = [&](u64 lo, u64 hi, std::map<u64, Link>& out) {
            for (u64 page = lo & ~u64{0xFFF}; page < hi; page += 0x1000) {
                const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
                if (!p)
                    continue;
                for (u32 at = 0; at < 0x1000; at += 8) {
                    u64 v;
                    std::memcpy(&v, p + at, 8);
                    if (v < heap_lo || v >= heap_hi)
                        continue;
                    auto it = std::upper_bound(targets.begin(), targets.end(), v + max_off);
                    if (it == targets.begin())
                        continue;
                    --it;
                    if (*it < v || *it - v > max_off)
                        continue;
                    out.emplace(page + at, Link{*it, *it - v});
                }
            }
        };
        for (int level = 0; level < depth && !targets.empty(); ++level) {
            std::sort(targets.begin(), targets.end());
            std::map<u64, Link> found;
            scan_range(main_lo, main_hi, found);
            scan_range(heap_lo, heap_hi, found);
            std::vector<u64> next;
            for (const auto& [holder, link] : found) {
                if (holder >= main_lo && holder < main_hi) {
                    // walk back down to the original target
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "main+%llx", static_cast<unsigned long long>(holder - main_lo));
                    std::string chain = buf;
                    u64 at = link.target;
                    std::snprintf(buf, sizeof buf, " +%llx", static_cast<unsigned long long>(link.offset));
                    chain += buf;
                    for (int l = level - 1; l >= 0; --l) {
                        const auto& h = levels[l].at(at);
                        std::snprintf(buf, sizeof buf, " ->+%llx", static_cast<unsigned long long>(h.offset));
                        chain += buf;
                        at = h.target;
                    }
                    const std::string msg = "Kirby DSMod ptrscan chain L" + std::to_string(level + 1) + ": " + chain;
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
                    ++statics;
                } else if (next.size() < 4096) {
                    next.push_back(holder);
                }
            }
            const std::string msg = "Kirby DSMod ptrscan level " + std::to_string(level + 1) + ": " +
                                    std::to_string(found.size()) + " holder(s)";
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
            levels.push_back(std::move(found));
            targets = std::move(next);
        }
        const std::string done = "Kirby DSMod ptrscan done: " + std::to_string(statics) + " static chain(s)";
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, done.c_str());
    }

    // Research: every 4-aligned place in the heap and main holding the u32 sequence in
    // user:findseq.txt (hex words).
    void FindSequence(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:findseq.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod findseq: no user:findseq.txt");
        std::vector<u32> want;
        const std::string line(text->begin(), text->end());
        for (std::size_t at = 0; at < line.size();) {
            char* end{};
            const unsigned long v = std::strtoul(line.c_str() + at, &end, 16);
            if (end == line.c_str() + at)
                break;
            want.push_back(static_cast<u32>(v));
            at = static_cast<std::size_t>(end - line.c_str());
        }
        if (want.empty() || want.size() > 256)
            return;
        const std::size_t bytes = want.size() * 4;
        int hits = 0;
        const auto scan = [&](u64 lo, u64 hi) {
            for (u64 page = lo & ~u64{0xFFF}; page < hi && hits < 64; page += 0x1000) {
                const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
                if (!p)
                    continue;
                for (u32 at = 0; at + bytes <= 0x1000; at += 4) {
                    if (std::memcmp(p + at, want.data(), bytes) != 0)
                        continue;
                    char buf[96];
                    const u64 where = page + at;
                    if (where >= host.main_base && where < host.main_base + host.main_size)
                        std::snprintf(buf, sizeof buf, "Kirby DSMod findseq hit main+%llx",
                                      static_cast<unsigned long long>(where - host.main_base));
                    else
                        std::snprintf(buf, sizeof buf, "Kirby DSMod findseq hit %llx",
                                      static_cast<unsigned long long>(where));
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, buf);
                    ++hits;
                }
            }
        };
        scan(host.main_base, host.main_base + host.main_size);
        scan(host.get_heap_begin(host.userdata), host.get_heap_end(host.userdata));
        const std::string done = "Kirby DSMod findseq done: " + std::to_string(hits) + " hit(s)";
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, done.c_str());
    }

    // Research: Magoland's Scn.Yy.BaseModules in the heap, told by its back references (its player manager's _mods at +0x10,
    // its pause manager's _modules at +0x20), with its create info manager (+0x10) and Grand Prix data manager (+0xA0)
    void BaseFind(const EdenDsmodHostApi& host) {
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        int hits = 0;
        for (u64 page = lo & ~u64{0xFFF}; page < hi && hits < 16; page += 0x1000) {
            const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
            if (!p)
                continue;
            for (u32 at = 0x10; at < 0x1000; at += 8) {
                u64 x, back{}, pause{}, pause_back{}, info{}, grand{};
                std::memcpy(&x, p + at, 8);
                if (x < lo || x >= hi || (x & 7) || !Rd(host, x + 0x18, &back) || back != page + at - 0x10 || !Rd(host, x + 0x40, &pause) ||
                    !Rd(host, pause + 0x20, &pause_back) || pause_back != x)
                    continue;
                Rd(host, x + 0x10, &info), Rd(host, x + 0xA0, &grand);
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                         ("Kirby DSMod basefind " + KirbyHex(x) + " info " + KirbyHex(info) + " grandprix " + KirbyHex(grand)).c_str());
                ++hits;
            }
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod basefind done: " + std::to_string(hits)).c_str());
    }

    // Research: rule-driven value scan. user:vscan.txt holds one command:
    //   init <lo hex> <hi hex> i|f <min> <max>   every 4-aligned value in range (int or float)
    //   keep inc|dec|same|changed                 compared with the value seen last time
    //   keep eq|lt|gt <v>                         compared with a constant
    //   track <n> [inc|dec]                       for the next n frames, keep values that only move
    //                                             that way and end past where they started
    //   track <n> pulse                           keep values that rise (3+ steps), fall back and
    //                                             end where they started
    void PulseStep(const EdenDsmodHostApi& host) {
        for (auto& [addr, p] : vscan_pulse) {
            if (p.dead)
                continue;
            u32 raw;
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, addr, &raw, 4)) {
                p.dead = true;
                continue;
            }
            const s32 now = static_cast<s32>(raw), last = static_cast<s32>(p.last);
            if (!p.falling) {
                if (now > last)
                    ++p.rises;
                else if (now < last)
                    p.falling = true, p.dead = p.rises < 3;
            } else if (now > last) {
                p.dead = true;
            }
            p.last = raw;
        }
        if (--vscan_track > 0)
            return;
        std::vector<std::pair<u64, u32>> keep;
        for (const auto& [addr, p] : vscan_pulse)
            if (!p.dead && p.falling && p.last == p.start)
                keep.push_back({addr, p.start});
        vscan.swap(keep);
        std::string msg = "Kirby DSMod vscan track done: " + std::to_string(vscan.size()) + " left";
        for (std::size_t i = 0; i < vscan.size() && i < 40; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " %llx(%d rises)", static_cast<unsigned long long>(vscan[i].first),
                          vscan_pulse[vscan[i].first].rises);
            msg += buf;
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    void TrackStep(const EdenDsmodHostApi& host) {
        if (!vscan_pulse.empty())
            return PulseStep(host);
        std::vector<std::pair<u64, u32>> keep;
        for (const auto& [addr, old] : vscan) {
            u32 raw;
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, addr, &raw, 4))
                continue;
            const auto less = [&](u32 x, u32 y) {
                return vscan_float ? std::bit_cast<float>(x) < std::bit_cast<float>(y)
                                   : static_cast<s32>(x) < static_cast<s32>(y);
            };
            const bool ok = vscan_track_down
                                ? !less(old, raw) && (vscan_track > 1 || less(raw, vscan_start[addr]))
                                : !less(raw, old) && (vscan_track > 1 || less(vscan_start[addr], raw));
            if (ok)
                keep.push_back({addr, raw});
        }
        vscan.swap(keep);
        if (--vscan_track == 0) {
            std::string msg = "Kirby DSMod vscan track done: " + std::to_string(vscan.size()) + " left";
            for (std::size_t i = 0; i < vscan.size() && i < 40; ++i) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " %llx=%d/%g", static_cast<unsigned long long>(vscan[i].first),
                              static_cast<s32>(vscan[i].second), std::bit_cast<float>(vscan[i].second));
                msg += buf;
            }
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
        }
    }

    void ValueScan(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:vscan.txt");
        if (!text)
            return host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod vscan: no user:vscan.txt");
        char op[16]{}, rule[16]{}, kind[4]{};
        unsigned long long lo{}, hi{};
        double a{}, b{};
        const std::string line(text->begin(), text->end());
        std::sscanf(line.c_str(), "%15s", op);
        const auto as_number = [&](u32 raw) {
            if (vscan_float) {
                float f;
                std::memcpy(&f, &raw, 4);
                return static_cast<double>(f);
            }
            return static_cast<double>(static_cast<s32>(raw));
        };
        if (std::string_view{op} == "track") {
            int frames{};
            char dir[8]{};
            std::sscanf(line.c_str(), "%*s %d %7s", &frames, dir);
            vscan_track_down = std::string_view{dir} == "dec";
            vscan_pulse.clear();
            if (std::string_view{dir} == "pulse")
                for (const auto& [addr, raw] : vscan)
                    vscan_pulse[addr] = {raw, raw, 0, false, false};
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, "Kirby DSMod vscan track started");
            vscan_track = std::clamp(frames, 1, 600);
            vscan_start.clear();
            for (const auto& [addr, raw] : vscan)
                vscan_start[addr] = raw;
            return;
        }
        if (std::string_view{op} == "init") {
            if (std::sscanf(line.c_str(), "%*s %llx %llx %3s %lf %lf", &lo, &hi, kind, &a, &b) != 5)
                return;
            vscan_float = kind[0] == 'f';
            vscan.clear();
            for (u64 page = lo & ~u64{0xFFF}; page < hi; page += 0x1000) {
                const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
                if (!p)
                    continue;
                for (u32 at = 0; at < 0x1000; at += 4) {
                    u32 raw;
                    std::memcpy(&raw, p + at, 4);
                    const double v = as_number(raw);
                    if (v >= a && v <= b)
                        vscan.push_back({page + at, raw});
                }
            }
        } else {
            std::sscanf(line.c_str(), "%*s %15s %lf", rule, &a);
            const std::string_view r{rule};
            std::vector<std::pair<u64, u32>> keep;
            for (const auto& [addr, old] : vscan) {
                u32 raw;
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, addr, &raw, 4))
                    continue;
                const double now = as_number(raw), was = as_number(old);
                const bool ok = r == "inc" ? now > was : r == "dec" ? now < was : r == "same" ? raw == old
                                : r == "changed" ? raw != old : r == "eq" ? now == a
                                : r == "lt" ? now < a : r == "gt" ? now > a : false;
                if (ok)
                    keep.push_back({addr, raw});
            }
            vscan.swap(keep);
        }
        std::string msg = "Kirby DSMod vscan " + std::string{op} + " " + std::string{rule} + ": " +
                          std::to_string(vscan.size()) + " left";
        for (std::size_t i = 0; i < vscan.size() && i < 40; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " %llx=%g", static_cast<unsigned long long>(vscan[i].first),
                          as_number(vscan[i].second));
            msg += buf;
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    // Research: which object a main static (or a slot of the object it points to) refers to,
    // identified by its vtable. user:vtscan.txt: init | same | changed | show.
    std::optional<u64> VtableOf(const EdenDsmodHostApi& host, u64 obj) const {
        u64 vt{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, obj, &vt, 8))
            return std::nullopt;
        if (vt < host.main_base + 0x110b000 || vt >= host.main_base + 0x1620000)
            return std::nullopt;
        return vt - host.main_base;
    }

    std::optional<u64> VtAt(const EdenDsmodHostApi& host, const VtSlot& s) const {
        u64 obj{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, s.at, &obj, 8))
            return std::nullopt;
        if (s.off >= 0 && !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, obj + s.off, &obj, 8))
            return std::nullopt;
        if (s.off2 >= 0 && !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, obj + s.off2, &obj, 8))
            return std::nullopt;
        return VtableOf(host, obj);
    }

    void VtableScan(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:vtscan.txt");
        if (!text)
            return;
        const std::string op(text->begin(), std::find_if(text->begin(), text->end(), [](u8 c) { return c <= ' '; }));
        const u64 heap_lo = host.get_heap_begin(host.userdata), heap_hi = host.get_heap_end(host.userdata);
        if (op == "init") {
            vtscan.clear();
            for (u64 at = host.main_base + 0x1542000; at < host.main_base + host.main_size; at += 8) {
                u64 obj{};
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &obj, 8) ||
                    obj < heap_lo || obj >= heap_hi)
                    continue;
                if (const auto vt = VtableOf(host, obj))
                    vtscan.push_back({at, -1, -1, *vt});
                const u8* p = host.get_read_pointer(host.userdata, obj, 0x200);
                if (!p)
                    continue;
                for (s32 off = 0; off < 0x200; off += 8) {
                    u64 child;
                    std::memcpy(&child, p + off, 8);
                    if (child < heap_lo || child >= heap_hi)
                        continue;
                    if (const auto vt = VtableOf(host, child))
                        vtscan.push_back({at, off, -1, *vt});
                    const u8* q = host.get_read_pointer(host.userdata, child, 0x100);
                    if (!q)
                        continue;
                    for (s32 off2 = 0; off2 < 0x100; off2 += 8) {
                        u64 grand;
                        std::memcpy(&grand, q + off2, 8);
                        if (grand < heap_lo || grand >= heap_hi)
                            continue;
                        if (const auto vt = VtableOf(host, grand))
                            vtscan.push_back({at, off, off2, *vt});
                    }
                }
            }
        } else if (op == "same" || op == "changed") {
            std::vector<VtSlot> keep;
            for (auto s : vtscan) {
                const auto vt = VtAt(host, s);
                if (!vt || (*vt == s.vt) != (op == "same"))
                    continue;
                s.vt = *vt;
                keep.push_back(s);
            }
            vtscan.swap(keep);
        }
        std::string msg = "Kirby DSMod vtscan " + op + ": " + std::to_string(vtscan.size()) + " left";
        for (std::size_t i = 0; i < vtscan.size() && i < 60; ++i) {
            char buf[96];
            const auto vt = VtAt(host, vtscan[i]);
            std::snprintf(buf, sizeof buf, " main+%llx/%d/%d=%llx", static_cast<unsigned long long>(vtscan[i].at - host.main_base),
                          vtscan[i].off, vtscan[i].off2, static_cast<unsigned long long>(vt.value_or(0)));
            msg += buf;
        }
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    // Research: small-int scan over the whole heap, one byte per word. user:hscan.txt:
    // init | same | changed | eq <v> | base (back to the value at the last mark) | prev / next (one below / above
    // it) | mark | show <skip>.
    struct HPage {
        u64 page;
        std::array<u8, 1024> cur, base; // 255 = out
    };
    std::vector<HPage> hscan;

    void HeapScan(const EdenDsmodHostApi& host) {
        const auto text = romfs("user:hscan.txt");
        if (!text)
            return;
        const std::string line(text->begin(), text->end());
        char op[16]{};
        unsigned arg = 0;
        std::sscanf(line.c_str(), "%15s %u", op, &arg);
        HeapOp(host, op, arg, line);
    }

    // beq <v>: keep the words whose value at the last mark was v
    void HeapOp(const EdenDsmodHostApi& host, std::string_view o, unsigned arg, const std::string& line) {
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        if (o == "init") {
            hscan.clear();
            for (u64 page = lo & ~u64{0xFFF}; page < hi; page += 0x1000) {
                const u8* p = host.get_read_pointer(host.userdata, page, 0x1000);
                if (!p)
                    continue;
                HPage h{page, {}, {}};
                bool any = false;
                for (int i = 0; i < 1024; ++i) {
                    u32 w;
                    std::memcpy(&w, p + i * 4, 4);
                    h.cur[i] = w < 255 ? static_cast<u8>(w) : 255;
                    any |= h.cur[i] != 255;
                }
                h.base = h.cur;
                if (any)
                    hscan.push_back(h);
            }
        } else if (o == "mark") {
            for (auto& h : hscan)
                h.base = h.cur;
        } else if (o != "show") {
            std::vector<HPage> keep;
            for (auto& h : hscan) {
                const u8* p = host.get_read_pointer(host.userdata, h.page, 0x1000);
                if (!p)
                    continue;
                bool any = false;
                for (int i = 0; i < 1024; ++i) {
                    if (h.cur[i] == 255)
                        continue;
                    u32 w;
                    std::memcpy(&w, p + i * 4, 4);
                    const bool ok = w < 255 && (o == "same" ? w == h.cur[i] : o == "changed" ? w != h.cur[i]
                                                : o == "base" ? w == h.base[i] : o == "beq" ? h.base[i] == arg
                                                : o == "prev" ? w + 1 == h.base[i]
                                                : o == "next" ? w == h.base[i] + 1u : w == arg);
                    h.cur[i] = ok ? static_cast<u8>(w) : 255;
                    any |= ok;
                }
                if (any)
                    keep.push_back(h);
            }
            hscan.swap(keep);
        }
        std::size_t left = 0;
        std::string list;
        const std::size_t skip = o == "show" ? arg : 0;
        for (const auto& h : hscan)
            for (int i = 0; i < 1024; ++i)
                if (h.cur[i] != 255 && ++left > skip && left <= skip + 150) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, " %llx=%u/%u", static_cast<unsigned long long>(h.page + i * 4),
                                  h.cur[i], h.base[i]);
                    list += buf;
                }
        const std::string msg = "Kirby DSMod hscan " + line + ": " + std::to_string(left) + " left" + list;
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
    }

    static std::string fmt(u64 obj, u64 vt, u64 comp, u32 ability) {
        char buf[128];
        std::snprintf(buf, sizeof buf, " owner=%llx(%llx) C=%llx ability=%u",
                      static_cast<unsigned long long>(obj), static_cast<unsigned long long>(vt),
                      static_cast<unsigned long long>(comp), ability);
        return buf;
    }

    EdenDsmodBool LoadImage(const EdenDsmodHostApi* host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!host || !host->read_romfs || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        if (!k.starts_with(KeyPrefix))
            return EDEN_DSMOD_FALSE;
        const auto logical = k.substr(KeyPrefix.size());
        if (logical.starts_with("font/")) {
            const auto font = KirbyFont::BuildFontAtlas(romfs, logical.substr(5), true);
            if (!font)
                return EDEN_DSMOD_FALSE;
            sink(receiver, font->atlas.width, font->atlas.height, font->atlas.rgba.data(),
                 font->atlas.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/pausebg/")) {
            const auto size = logical.substr(12);
            const auto x = size.find('x');
            std::uint32_t w{}, h{};
            const auto mask = assets.Load(romfs, "lyt/Pause/Parts/PauseBg^t");
            if (x == std::string_view::npos || !mask ||
                std::from_chars(size.data(), size.data() + x, w).ec != std::errc{} ||
                std::from_chars(size.data() + x + 1, size.data() + size.size(), h).ec != std::errc{})
                return EDEN_DSMOD_FALSE;
            const auto bg = KirbyArt::PauseBackground(*mask, w, h);
            if (!bg)
                return EDEN_DSMOD_FALSE;
            sink(receiver, bg->width, bg->height, bg->rgba.data(), bg->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/titlebg/")) {
            const auto size = logical.substr(12);
            const auto x = size.find('x');
            std::uint32_t w{}, h{};
            const auto window = assets.Load(romfs, "lyt/Pause/Parts/PauseWinBase00^u");
            if (x == std::string_view::npos || !window ||
                std::from_chars(size.data(), size.data() + x, w).ec != std::errc{} ||
                std::from_chars(size.data() + x + 1, size.data() + size.size(), h).ec != std::errc{})
                return EDEN_DSMOD_FALSE;
            const auto bg = KirbyArt::TitleBackground(*window, w, h);
            if (!bg)
                return EDEN_DSMOD_FALSE;
            sink(receiver, bg->width, bg->height, bg->rgba.data(), bg->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/startbtn/")) {
            const auto button = StartButtonImage(logical.substr(13));
            if (!button)
                return EDEN_DSMOD_FALSE;
            sink(receiver, button->width, button->height, button->rgba.data(), button->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/files/")) {
            const auto image = FilesImage(logical.substr(10));
            if (!image)
                return EDEN_DSMOD_FALSE;
            {
                std::scoped_lock lock{files_mutex};
                files_served.insert(std::string{logical.substr(10)});
            }
            sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/logo/")) {
            // gen/logo/<msg dir>/<w>x<h>
            const auto rest = logical.substr(9);
            const auto slash = rest.find('/');
            const auto size = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
            const auto x = size.find('x');
            std::uint32_t w{}, h{};
            if (x == std::string_view::npos ||
                std::from_chars(size.data(), size.data() + x, w).ec != std::errc{} ||
                std::from_chars(size.data() + x + 1, size.data() + size.size(), h).ec != std::errc{})
                return EDEN_DSMOD_FALSE;
            const auto logo = TitleLogoImage(rest.substr(0, slash), w, h);
            if (!logo)
                return EDEN_DSMOD_FALSE;
            sink(receiver, logo->width, logo->height, logo->rgba.data(), logo->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("mask/")) {
            // ^t textures are alpha masks over black: white RGB so the widget colour tints them
            const auto mask = assets.Load(romfs, logical.substr(5));
            if (!mask)
                return EDEN_DSMOD_FALSE;
            auto rgba = mask->rgba;
            for (std::size_t i = 0; i < rgba.size(); i += 4)
                rgba[i] = rgba[i + 1] = rgba[i + 2] = 0xFF;
            sink(receiver, mask->width, mask->height, rgba.data(), rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        const auto image = assets.Load(romfs, logical);
        if (!image) {
            if (host->log)
                host->log(host->userdata, EDEN_DSMOD_LOG_WARNING,
                          ("Kirby DSMod: cannot load " + std::string{k}).c_str());
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    // The game's language: its nn::settings language code at main+0x16324F0 ("it", "en-US", ...),
    // mapped to the msg/Ark folder the game reads.
    static std::string_view MessageDir(std::string_view code) {
        static constexpr std::pair<std::string_view, std::string_view> Dirs[]{
            {"ja", "JP_Japanese"},    {"en-US", "US_English"},     {"en-GB", "EU_English"},
            {"fr", "EU_French"},      {"fr-CA", "US_French"},      {"de", "EU_German"},
            {"it", "EU_Italian"},     {"es", "EU_Spanish"},        {"es-419", "US_Spanish"},
            {"nl", "EU_Dutch"},       {"pt", "US_Portuguese"},     {"pt-BR", "US_Portuguese"},
            {"ko", "KR_Korean"},      {"zh-CN", "CN_Chinese"},     {"zh-Hans", "CN_Chinese"},
            {"zh-TW", "TW_Chinese"},  {"zh-Hant", "TW_Chinese"},
        };
        for (const auto& [c, dir] : Dirs)
            if (c == code)
                return dir;
        return "US_English";
    }

    std::string GameMessageDir(const EdenDsmodHostApi& host) const {
        char code[9]{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x16324F0, code, 8))
            return {};
        return std::string{MessageDir(code)};
    }

    std::optional<KirbyAssets::Image> StartButtonImage(std::string_view dir) {
        static constexpr float Scale = 1.5f; // companion px per MainTitle layout unit
        if (dir.empty() || dir.size() > 20 || dir.find('/') != std::string_view::npos)
            return std::nullopt;
        const auto msbt = romfs("/msg/Ark/" + std::string{dir} + "/TitleMenu.msbt");
        const auto text = msbt ? KirbyAssets::MsbtText(*msbt, "TitleButton") : std::nullopt;
        const auto font = KirbyFont::BuildFontAtlas(romfs, "FOT-RodinWanpakuPro-EB:43");
        const auto cap = assets.Load(romfs, "lyt/TitleMenu/Body/MainTitleStartBaseBase1^t");
        const auto fill = assets.Load(romfs, "lyt/TitleMenu/Body/MainTitleStartBaseBase2^u");
        const auto disc = assets.Load(romfs, "lyt/Resident/ControllerBtnIcon/Parts/CmnBtnIconBase^t");
        const auto glyph = assets.Load(romfs, "lyt/Resident/ControllerBtnIcon/Parts/CmnBtnIconA^t");
        if (!text || !font || !cap || !fill || !disc || !glyph)
            return std::nullopt;
        return KirbyArt::StartButton({*cap, *fill, *disc, *glyph, *font, *text}, Scale);
    }

    // MainTitleLogo still: the TitleMenu/PartsLang archive of the game's language, its base and
    // frame pictures (no shine, glitter or star animation).
    static std::string_view LogoArchive(std::string_view dir) {
        static constexpr std::pair<std::string_view, std::string_view> Archives[]{
            {"JP_Japanese", "Lang"},           {"US_English", "Lang.US_ENG"},   {"EU_English", "Lang.EU_ENG"},
            {"EU_French", "Lang.EU_FRA"},      {"US_French", "Lang.US_FRA"},    {"EU_German", "Lang.EU_GER"},
            {"EU_Italian", "Lang.EU_ITA"},     {"EU_Spanish", "Lang.EU_SPA"},   {"US_Spanish", "Lang.US_SPA"},
            {"EU_Dutch", "Lang.EU_NLD"},       {"US_Portuguese", "Lang.US_POR"}, {"KR_Korean", "Lang.KR"},
            {"CN_Chinese", "Lang.CN"},         {"TW_Chinese", "Lang.TW"},
        };
        for (const auto& [d, archive] : Archives)
            if (d == dir)
                return archive;
        return "Lang.US_ENG";
    }

    std::optional<KirbyAssets::Image> TitleLogoImage(std::string_view dir, std::uint32_t w, std::uint32_t h) {
        static constexpr std::string_view Panes[]{"P_PopStar",  "P_MainBase", "P_MainBaseStar",
                                                  "P_MainFrame", "P_DXBase00", "P_DXBase01", "P_DXLight",
                                                  "P_DXKira1", "P_DXKira2", "P_DXFrame"};
        const std::string archive = "lyt/TitleMenu/PartsLang/" + std::string{LogoArchive(dir)};
        const auto packed = romfs("/" + archive + ".arc.cmp");
        const auto sarc = packed ? KirbyAssets::DecompressCmp(*packed) : std::nullopt;
        const auto bflyt = sarc ? KirbyAssets::SarcMember(*sarc, "blyt/MainTitleLogo.bflyt") : std::nullopt;
        const auto layout = bflyt ? KirbyLayout::Parse(*bflyt) : std::nullopt;
        if (!layout)
            return std::nullopt;
        return KirbyArt::LayoutStill(*layout, Panes,
                                     [&](const std::string& name) { return assets.Load(romfs, archive + "/" + name); },
                                     w, h, KirbyArt::Anchor::BottomRight);
    }

    // File select page. Geometry shared with tools/gen_manifest.py (FILE_* constants).
    static constexpr std::uint32_t SlotX = 76, SlotW = 1088, SlotH = 292, SlotEnd = 156, CursorMargin = 10;
    static constexpr std::uint32_t SlotY[3]{72, 394, 716};
    static constexpr std::uint32_t KirbySize = 56, LabelW = 240, LabelH = 54, PctW = 240, PctH = 60;
    static constexpr std::uint32_t ModesW = 960, ModesH = 184, ModePctW = 380, ModePctH = 64;
    static constexpr int RainbowFrames = 180; // FileSelectBtn_FocusWait: one turn in 180 frames
    static constexpr int RainbowSteps = 90;   // rows per turn: one every 2 frames, the host's 30 Hz republish
    static constexpr std::uint32_t CanvasW = 1240;

    std::mutex layouts_mutex;
    std::map<std::string, std::optional<KirbyLayout::Layout>> layouts;

    // A layout from a romfs layout archive ("TitleMenu/Parts"), parsed once.
    std::optional<KirbyLayout::Layout> Layout(const std::string& archive, const std::string& name) {
        std::scoped_lock lock{layouts_mutex};
        const std::string id = archive + "/" + name;
        if (const auto it = layouts.find(id); it != layouts.end())
            return it->second;
        const auto packed = romfs("/lyt/" + archive + ".arc.cmp");
        const auto sarc = packed ? KirbyAssets::DecompressCmp(*packed) : std::nullopt;
        const auto bflyt = sarc ? KirbyAssets::SarcMember(*sarc, "blyt/" + name + ".bflyt") : std::nullopt;
        return layouts[id] = bflyt ? KirbyLayout::Parse(*bflyt) : std::nullopt;
    }

    KirbyFiles::Sources FilesSources() {
        return {[this](const std::string& n) { return assets.Load(romfs, "lyt/TitleMenu/Parts/" + n); },
                [this](const std::string& n) { return Layout("TitleMenu/Parts", n); },
                [this](const std::string& spec) { return KirbyFont::BuildFontAtlas(romfs, spec); }};
    }

    KirbyModes::Sources ModesSources() {
        return {FilesSources(), [this](const std::string& path) { return assets.Load(romfs, path); },
                [this](const std::string& archive, const std::string& name) { return Layout(archive, name); }};
    }

    // mode/<msg dir>/<story>/<extra>/<epilogue>/<magoland>: dot-separated numbers, as ModesKey writes them
    std::optional<KirbyAssets::Image> RenderModes(const std::vector<std::string>& f) {
        const auto numbers = [](const std::string& field) {
            std::vector<int> v;
            for (std::size_t a = 0, b; a <= field.size(); a = b + 1) {
                b = std::min(field.find('.', a), field.size());
                v.push_back(std::atoi(field.substr(a, b - a).c_str()));
            }
            return v;
        };
        const auto s = numbers(f[2]), e = numbers(f[3]), epi = numbers(f[4]), mago = numbers(f[5]);
        const auto page = TitlePage();
        if (s.size() != 6 || e.size() != 6 || epi.size() != 3 || mago.size() != 2 + KirbyModes::Attractions || !page)
            return std::nullopt;
        const auto mode = [](const std::vector<int>& v) { return KirbyModes::Mode{v[0] != 0, v[1], v[2], v[3], v[4], v[5]}; };
        KirbyModes::Magoland magoland{mago[0] != 0, mago[1] != 0, {}};
        std::copy(mago.begin() + 2, mago.end(), magoland.done.begin());
        const auto msg = [&](const std::string& file, const char* label) {
            const auto m = romfs("/msg/Ark/" + f[1] + "/" + file + ".msbt");
            const auto t = m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
            return t ? *t : std::u32string{};
        };
        KirbyModes::Texts texts{msg("TitleMenu", "ModeSelectStory"), msg("TitleMenu", "ModeSelectStoryExtra"),
                                msg("Ship", "MahoroaName"), {}};
        static constexpr const char* Games[]{"EggCatcher", "Setsuna", "Ochiochi", "Rally", "Punch",
                                             "Ride", "Hayauchi", "Shuriken", "Otsukai", "Tps"};
        for (int i = 0; i < 10; ++i)
            texts.titles[i] = msg("Name/YySubGame", Games[i]);
        texts.titles[10] = msg("YyMission", "Yy");
        // "final" on a line of its own, or the ticket's text gets too small
        texts.titles[11] = msg("YyMission", "Extra");
        if (const auto space = texts.titles[11].rfind(U' ');
            space != std::u32string::npos && texts.titles[11].find(U'\n') == std::u32string::npos)
            texts.titles[11][space] = U'\n';
        return KirbyModes::Page(ModesSources(), *page, texts, mode(s), mode(e), {epi[0] != 0, epi[1] != 0, epi[2], 0},
                                magoland);
    }

    std::optional<KirbyAssets::Image> MainMenu() {
        const auto window = assets.Load(romfs, "lyt/Pause/Parts/PauseWinBase00^u");
        const auto checker = window ? KirbyArt::TitleBackground(*window, 1240, 1080, false) : std::nullopt;
        return checker ? KirbyModes::MainMenuPage(ModesSources(), *checker) : std::nullopt;
    }

    // advp/<msg dir>/<mode>/<lives>.<stars>.<parts>.<open>/<got per world>/<of per world>/<beaten per world>/<medals>
    std::optional<KirbyAssets::Image> RenderAdventure(const std::vector<std::string>& f) {
        const auto numbers = [](const std::string& field) {
            std::vector<int> v;
            for (std::size_t a = 0, b; a <= field.size(); a = b + 1) {
                b = std::min(field.find('.', a), field.size());
                v.push_back(std::atoi(field.substr(a, b - a).c_str()));
            }
            return v;
        };
        const auto n = numbers(f[3]), got = numbers(f[4]), of = numbers(f[5]), beaten = numbers(f[6]), medals = numbers(f[7]);
        const auto map = romfs("/msg/Ark/" + f[1] + "/WorldMap.msbt");
        if (n.size() != 4 || got.size() != KirbyModes::Worlds || of.size() != KirbyModes::Worlds ||
            beaten.size() != KirbyModes::Worlds || medals.size() != KirbyModes::Challenges || !map)
            return std::nullopt;
        KirbyModes::Adventure a{f[2] == "1", {}, n[0], n[1], n[2], n[3], {}};
        for (int w = 0; w < KirbyModes::Worlds; ++w) {
            const std::string lv = "LevelName_Lv" + std::to_string(w + 1);
            const auto number = KirbyAssets::MsbtText(*map, lv);
            const auto name = KirbyAssets::MsbtRuns(*map, lv + "Title");
            a.worlds[w] = {number ? *number : std::u32string{}, name ? *name : std::vector<KirbyAssets::MsbtRun>{}, got[w], of[w],
                           beaten[w] != 0};
        }
        std::copy(medals.begin(), medals.end(), a.medals.begin());
        return KirbyModes::AdventurePanel(ModesSources(), a);
    }

    static std::string WorldCursorKey(const KirbyModes::MapSlot& slot) {
        return "wcur/" + std::to_string(std::lround(slot.disc * 10)); // the disc in tenths of a pixel
    }

    // An ability's star: its icon (AbilityStarIcon<icon>) and its colours, from its own animation
    // (CmnAbilityStarIcon_<key>; the layout's default is Sword's greens)
    struct Star {
        std::string icon;
        KirbyPaint::Rgb black{5, 193, 35}, white{0, 40, 15};
    };
    Star AbilityStar(const std::string& key) {
        Star star{key == "Crash" ? "Clash" : key == "Meta" ? "Metaknight" : key == "Dee" ? "WaddledeeBandana" : key.empty() ? "Normal" : key};
        const auto archive = romfs("/lyt/Cmn/AbilityStarIcon/Parts.arc.cmp");
        const auto sarc = archive ? KirbyAssets::DecompressCmp(*archive) : std::nullopt;
        const auto flan = sarc ? KirbyAssets::SarcMember(*sarc, "anim/CmnAbilityStarIcon_" + key + ".bflan") : std::nullopt;
        if (const auto c = flan ? KirbyAssets::BflanMaterialColour(*flan, "P_AbilityStarIcon") : std::nullopt) {
            const auto at = [&](int i, float fallback) { return (*c)[i] >= 0 ? (*c)[i] : fallback; };
            star.black = {at(0, star.black.r), at(1, star.black.g), at(2, star.black.b)};
            star.white = {at(4, star.white.r), at(5, star.white.g), at(6, star.white.b)};
        }
        return star;
    }

    // an effect file (ptcl/Step/Chara/<effect>.ptcl) decompressed once, and what Kirby's figures take from it: a texture
    // of its BNTX, a primitive (a model of the BFRES it embeds)
    std::map<std::string, std::shared_ptr<const KirbyAssets::Bytes>> effect_files;
    std::map<std::string, std::shared_ptr<const KirbyAssets::Image>> effect_textures;
    std::map<std::string, std::shared_ptr<const KirbyChara::Model>> effect_models;
    std::shared_ptr<const KirbyAssets::Bytes> EffectFile(const std::string& effect) {
        {
            std::scoped_lock lock{chara_mutex};
            if (const auto it = effect_files.find(effect); it != effect_files.end())
                return it->second;
        }
        std::shared_ptr<const KirbyAssets::Bytes> bytes;
        if (const auto raw = romfs("/ptcl/Step/Chara/" + effect + ".ptcl.cmp"))
            if (auto unpacked = KirbyAssets::DecompressCmp(*raw))
                bytes = std::make_shared<const KirbyAssets::Bytes>(std::move(*unpacked));
        std::scoped_lock lock{chara_mutex};
        return effect_files.emplace(effect, bytes).first->second;
    }
    // (the part of the file from its magic on: "BNTX", "FRES")
    static std::optional<std::span<const std::uint8_t>> EffectSection(const KirbyAssets::Bytes& bytes, std::string_view magic) {
        const std::string_view all{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        const auto at = all.find(magic);
        return at == std::string_view::npos ? std::nullopt : std::optional{std::span<const std::uint8_t>{bytes}.subspan(at)};
    }
    std::shared_ptr<const KirbyAssets::Image> EffectTexture(const std::string& effect, const std::string& texture) {
        if (texture.empty())
            return nullptr;
        const std::string key = effect + "/" + texture;
        {
            std::scoped_lock lock{chara_mutex};
            if (const auto it = effect_textures.find(key); it != effect_textures.end())
                return it->second;
        }
        std::shared_ptr<const KirbyAssets::Image> image;
        if (const auto bytes = EffectFile(effect))
            if (const auto bntx = EffectSection(*bytes, "BNTX"))
                if (auto decoded = KirbyAssets::DecodeBntx(*bntx, texture))
                    image = std::make_shared<const KirbyAssets::Image>(std::move(*decoded));
        std::scoped_lock lock{chara_mutex};
        return effect_textures.emplace(key, image).first->second;
    }
    std::shared_ptr<const KirbyChara::Model> EffectModel(const std::string& effect, const std::string& primitive) {
        const std::string key = effect + "/" + primitive;
        {
            std::scoped_lock lock{chara_mutex};
            if (const auto it = effect_models.find(key); it != effect_models.end())
                return it->second;
        }
        std::shared_ptr<const KirbyChara::Model> model;
        if (const auto bytes = EffectFile(effect))
            if (const auto fres = EffectSection(*bytes, "FRES"))
                model = std::make_shared<const KirbyChara::Model>(*fres, primitive);
        std::scoped_lock lock{chara_mutex};
        return effect_models.emplace(key, model).first->second;
    }

    // The Arenas' figures: models and motions (gfx/Step/Chara/...) parsed once, then posed and drawn by
    // KirbyChara on the drawing workers
    std::mutex chara_mutex;
    std::map<std::string, std::shared_ptr<const KirbyChara::Model>> chara_models;
    std::map<std::string, std::shared_ptr<const KirbyChara::Motion>> chara_motions;
    template <typename T>
    std::shared_ptr<const T> CharaFile(std::map<std::string, std::shared_ptr<const T>>& cache, const std::string& path) {
        {
            std::scoped_lock lock{chara_mutex};
            if (const auto it = cache.find(path); it != cache.end())
                return it->second;
        }
        const auto raw = romfs("/" + path + ".bfres.cmp");
        const auto bytes = raw ? KirbyAssets::DecompressCmp(*raw) : std::nullopt;
        if (!bytes)
            return nullptr;
        auto file = std::make_shared<const T>(*bytes);
        std::scoped_lock lock{chara_mutex};
        return cache.emplace(path, std::move(file)).first->second;
    }
    KirbyChara::Textures CharaTextures(const std::string& model) {
        return [this, model](const std::string& name) { return assets.Load(romfs, model + "/" + name); };
    }

    // a loop's frames framed alike (the figure would breathe in and out of its picture otherwise), each `frames` / 12
    // of the loop apart
    static constexpr int ArenaFrames = 12;
    std::optional<KirbyAssets::Image> DrawLoop(const std::function<std::vector<KirbyChara::Part>(float)>& posed, int frames, int frame,
                                              std::size_t framed, const KirbyChara::View& view, std::uint32_t size) {
        std::optional<KirbyChara::Framing> fit;
        for (int f = 0; f < ArenaFrames; ++f) {
            const auto parts = posed(static_cast<float>(frames * f) / ArenaFrames);
            const auto b = KirbyChara::Bounds({parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(std::min(framed, parts.size()))}, view);
            if (b)
                fit = fit ? KirbyChara::Union(*fit, *b) : *b;
        }
        if (!fit)
            return std::nullopt;
        return KirbyChara::Draw(posed(static_cast<float>(frames * frame) / ArenaFrames), view, *fit, size, 2);
    }

    // the other characters' figures (arenakirby/Meta, Dedede, Dee): their Wait (their weapon in it), turned towards the
    // boss, in the same panel picture as Kirby's
    std::optional<KirbyAssets::Image> RenderArenaCharacter(const std::string& folder, int frame) {
        const std::string root = "gfx/Step/Chara/" + folder + "/";
        const auto model = CharaFile(chara_models, root + "Model");
        const auto motion = CharaFile(chara_motions, root + "Motion");
        if (!model || !motion || !motion->Has("Wait"))
            return std::nullopt;
        const auto face = motion->Patterns("Wait_tp");
        const auto look = KirbyChara::BossLook(folder);
        const auto posed = [&](float f) {
            KirbyChara::Part part{model.get(), motion->Sample("Wait", f), CharaTextures(root + "Model"), "", face};
            KirbyChara::Apply(part, look);
            return std::vector<KirbyChara::Part>{part};
        };
        const std::uint32_t s = KirbyModes::ArenaKirbyPicture;
        const auto picture = DrawLoop(posed, motion->Frames("Wait"), frame, 1, KirbyChara::CharacterView(folder), s);
        if (!picture)
            return std::nullopt;
        const auto size = KirbyModes::PlayerPanelSize;
        return KirbyModes::PanelPicture(KirbyModes::Plate::Player, size[0], size[1], *picture, KirbyModes::ArenaKirbyCentre[0] - s / 2.0f,
                                        KirbyModes::ArenaKirbyCentre[1] - s / 2.0f, static_cast<float>(s));
    }

    // arenakirby/<ability>[.<colours>]/<frame>: Kirby's battle stance (StandReadyR's loop, with its angry face, mirrored to
    // face the boss) with the ability's hat on HatL and its weapons in hand (KirbyChara::KirbyAbility), in the spray's
    // colours (FigureToken). Framed on his body alone, so he keeps one size and place with every ability; the picture is
    // his panel's, cut along its rim
    std::optional<KirbyAssets::Image> RenderArenaKirby(const std::string& key, int frame) {
        const std::string ability = key.substr(0, key.find('.'));
        if (const std::string folder{KirbyChara::CharacterFolder(ability)}; !folder.empty())
            return RenderArenaCharacter(folder, frame);
        KirbyChara::Part body{};
        if (const auto dot = key.find('.'); dot != std::string::npos && key.size() == dot + 1 + 6 * body.kirby.size())
            for (std::size_t i = 0; i < body.kirby.size(); ++i)
                body.kirby[i] = static_cast<std::uint32_t>(std::strtoul(key.substr(dot + 1 + 6 * i, 6).c_str(), nullptr, 16));
        static const std::string Root = "gfx/Step/Chara/Kirby/";
        const auto model = CharaFile(chara_models, Root + "Base/Model");
        const auto motion = CharaFile(chara_motions, Root + "Normal/Motion");
        if (!model || !motion)
            return std::nullopt;
        const auto kit = KirbyChara::KirbyAbility(ability);
        const std::string dir = Root + ability + "/";
        const auto hat = kit.hat.empty() ? nullptr : CharaFile(chara_models, dir + kit.hat);
        const auto hat_motion = hat ? CharaFile(chara_motions, dir + kit.hat) : nullptr;
        struct Held {
            std::shared_ptr<const KirbyChara::Model> model;
            std::shared_ptr<const KirbyChara::Motion> motion;
            const KirbyChara::Gear* gear;
        };
        std::vector<Held> held;
        for (const auto& g : kit.gear)
            if (auto m = CharaFile(chara_models, dir + g.file))
                held.push_back({std::move(m), CharaFile(chara_motions, dir + g.file), &g});
        struct Effect {
            const KirbyChara::EffectModel* spec;
            std::shared_ptr<const KirbyChara::Model> model;
            std::shared_ptr<const KirbyAssets::Image> texture, mask, colour;
        };
        std::vector<Effect> effects;
        for (const auto& e : kit.effects)
            if (auto m = EffectModel(e.effect, e.primitive); m && m->Valid())
                if (auto t = EffectTexture(e.effect, e.texture))
                    effects.push_back({&e, std::move(m), std::move(t), EffectTexture(e.effect, e.mask), EffectTexture(e.effect, e.colour)});
        const std::string anim = "StandReadyR";
        const int frames = motion->Frames(anim);
        const auto face = motion->Patterns(anim + "_tp");
        const auto posed = [&](float f) {
            KirbyChara::Part kirby{model.get(), motion->Sample(anim, f), CharaTextures(Root + "Base/Model"), "", face};
            kirby.kirby = body.kirby;
            std::vector<KirbyChara::Part> parts{std::move(kirby)};
            // (the hat's and the weapons' anims stretched over the loop; the effects turned and moved six steps a loop)
            const float loop = f / static_cast<float>(std::max(1, frames)), step = loop * 6;
            if (hat) {
                KirbyChara::Part p{hat.get(), hat_motion ? KirbyChara::HatPose(kit, *hat, *hat_motion, loop) : KirbyChara::Pose{}, CharaTextures(dir + kit.hat),
                                   "HatL", {}, kit.hat_hide, kit.hat_tint};
                p.paint = kit.hat_paint;
                p.glass = kit.hat_glass;
                parts.push_back(std::move(p));
            }
            for (const auto& e : effects) {
                KirbyChara::Part p{e.model.get(), KirbyChara::EffectPose(*e.model, e.spec->phase + e.spec->spin * step),
                                   [](const std::string&) { return std::shared_ptr<const KirbyAssets::Image>{}; }, "HatL", {}};
                p.turn = e.spec->turn;
                p.effect = e.texture, p.effect_mask = e.mask, p.effect_colour = e.colour;
                p.scroll = {e.spec->scroll[0] * step, e.spec->scroll[1] * step}, p.effect_tile = e.spec->tile;
                p.effect_dark = e.spec->dark, p.effect_light = e.spec->light, p.effect_opacity = e.spec->opacity;
                parts.push_back(std::move(p));
            }
            for (const auto& h : held) {
                KirbyChara::Part p{h.model.get(), h.gear->anim.empty() || !h.motion ? KirbyChara::Pose{} : KirbyChara::GearPose(*h.motion, loop, h.gear->anim),
                                   CharaTextures(dir + h.gear->file), h.gear->bone, {}, h.gear->hide};
                p.turn = h.gear->turn, p.glass = h.gear->glass, p.matcap = h.gear->matcap, p.paint = h.gear->paint;
                parts.push_back(std::move(p));
            }
            return parts;
        };
        const std::uint32_t s = KirbyModes::ArenaKirbyPicture;
        const auto picture = DrawLoop(posed, frames, frame, 1, {-28, 8, 0.018f, true, KirbyModes::ArenaKirbyFill}, s);
        if (!picture)
            return std::nullopt;
        const auto size = KirbyModes::PlayerPanelSize;
        return KirbyModes::PanelPicture(KirbyModes::Plate::Player, size[0], size[1], *picture, KirbyModes::ArenaKirbyCentre[0] - s / 2.0f,
                                        KirbyModes::ArenaKirbyCentre[1] - s / 2.0f, static_cast<float>(s));
    }

    // a boss in its idle loop (Wait, else Wait1, FlightWait; Move where the idle stands still, as Moundo's), facing Kirby;
    // a model without motions of its own (Super Bonkers, some of the True Arena's) takes its plain kind's. A folder with a
    // path (Lowper/Model/LowperLv2) names the model itself, its motions in its first folder.
    std::pair<std::shared_ptr<const KirbyChara::Motion>, std::string> BossIdle(const std::string& folder) {
        auto motion = CharaFile(chara_motions, "gfx/Step/Chara/" + folder.substr(0, folder.find('/')) + "/Motion");
        for (std::string plain = folder; !motion && !plain.empty();) {
            if (plain.ends_with("Ex"))
                plain.resize(plain.size() - 2);
            else if (plain.starts_with("Sp"))
                plain = plain.substr(2);
            else
                break;
            motion = CharaFile(chara_motions, "gfx/Step/Chara/" + plain + "/Motion");
        }
        std::string anim;
        for (const char* a : {"Wait", "Wait1", "FlightWait"})
            if (motion && anim.empty() && motion->Has(a))
                anim = a;
        if (motion && (anim.empty() || !motion->Moves(anim)) && motion->Has("Move"))
            anim = "Move";
        return {motion, anim};
    }

    std::optional<KirbyAssets::Image> RenderBossFigure(const std::string& folder, int frame, std::uint32_t size, float fill = 0) {
        const bool path = folder.find('/') != std::string::npos;
        const std::string root = "gfx/Step/Chara/" + folder, model_at = path ? root : root + "/Model";
        const auto model = CharaFile(chara_models, model_at);
        const auto [motion, anim] = BossIdle(folder);
        if (!model || !motion || anim.empty())
            return std::nullopt;
        auto look = KirbyChara::BossLook(folder.substr(folder.rfind('/') + 1));
        if (fill > 0)
            look.view.fill = fill, look.view.dx = look.view.dy = 0;
        const auto posed = [&](float f) {
            KirbyChara::Part part{model.get(), motion->Sample(anim, f), CharaTextures(model_at), "", motion->Patterns(anim + "_tp")};
            KirbyChara::Apply(part, look);
            return std::vector<KirbyChara::Part>{part};
        };
        return DrawLoop(posed, motion->Frames(anim), frame, 1, look.view, size);
    }

    // arenaboss/<round>/<frame>: the boss's card picture, as big as its panel and cut along the panel's rim: one boss,
    // or a group side by side (the middle ones of three or more higher and behind), four of one in two rows
    std::optional<KirbyAssets::Image> RenderArenaBoss(const std::string& round, int frame) {
        const auto members = ArenaMembers(round);
        const u32 w = KirbyModes::BossPanelSize[0], h = KirbyModes::BossPanelSize[1];
        KirbyAssets::Image out{w, h, std::vector<u8>(static_cast<std::size_t>(w) * h * 4)};
        const auto put = [&](const KirbyAssets::Image& figure, float x, float y, float size) {
            const auto part = KirbyModes::PanelPicture(KirbyModes::Plate::Boss, w, h, figure, x, y, size);
            for (std::size_t k = 0; k < out.rgba.size(); k += 4) {
                const u8* c = &part.rgba[k];
                if (!c[3])
                    continue;
                u8* d = &out.rgba[k];
                const float a = c[3] / 255.0f, keep = d[3] / 255.0f * (1 - a), all = a + keep;
                for (int j = 0; j < 3; ++j)
                    d[j] = static_cast<u8>((c[j] * a + d[j] * keep) / all + 0.5f);
                d[3] = static_cast<u8>(all * 255 + 0.5f);
            }
        };
        const std::size_t n = members.size();
        if (n == 1) {
            const auto one = RenderBossFigure(members.front(), frame, KirbyModes::ArenaBossSize);
            if (!one)
                return std::nullopt;
            put(*one, 110, -85, static_cast<float>(KirbyModes::ArenaBossSize));
            return out;
        }
        const bool square = n == 4 && std::all_of(members.begin(), members.end(), [&](const auto& m) { return m == members.front(); });
        const float size = square ? 300.0f : n == 2 ? 360.0f : n == 3 ? 290.0f : 250.0f;
        // (the panel shows from x 90 to 600 at its middle, above the plate from y 18 to 340)
        std::vector<std::size_t> order;
        for (std::size_t i = 1; i < n; i += 2)
            order.push_back(i);
        for (std::size_t i = 0; i < n; i += 2)
            order.push_back(i);
        if (square)
            order = {0, 1, 2, 3};
        std::optional<KirbyAssets::Image> one;
        for (const std::size_t i : order) {
            if (!square || !one)
                one = RenderBossFigure(members[i], frame, static_cast<u32>(size), 1.05f);
            if (!one)
                continue;
            const float x = square ? 120.0f + static_cast<float>(i % 2) * 200 + static_cast<float>(i / 2) * 50
                                   : 55.0f + static_cast<float>(i) * (600 - 55 - size + 40) / static_cast<float>(n - 1);
            const float y = square ? -38.0f + static_cast<float>(i / 2) * 140 : n > 2 && i % 2 ? 5.0f : 340 - size + 10;
            put(*one, x, y, size);
        }
        return out;
    }

    // Merry Magoland's sections: the game's own names for the missions and the masks, ours for the others
    std::u32string MagoName(const std::string& dir, int section) {
        const auto msg = [&](const char* file, const char* label) -> std::optional<std::u32string> {
            const auto m = romfs("/msg/Ark/" + dir + "/" + file + ".msbt");
            return m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
        };
        if (const auto t = section == 1 ? msg("YyCmn", "$MissionListHeadline") : section == 2 ? msg("YmEntrance", "DialogBtn_Omen") : std::nullopt;
            t && !t->empty())
            return *t;
        struct Own {
            const char* lang;
            const char32_t *games, *missions, *masks, *cards;
        };
        static constexpr Own Ours[]{{"Italian", U"Minigiochi", U"Missioni", U"Maschere", U"Tessere"},
                                    {"English", U"Minigames", U"Missions", U"Masks", U"Stamp Cards"},
                                    {"French", U"Mini-jeux", U"Missions", U"Masques", U"Cartes"},
                                    {"German", U"Minispiele", U"Missionen", U"Masken", U"Stempelkarten"},
                                    {"Spanish", U"Minijuegos", U"Misiones", U"Máscaras", U"Tarjetas"},
                                    {"Dutch", U"Minigames", U"Missies", U"Maskers", U"Stempelkaarten"},
                                    {"Portuguese", U"Minijogos", U"Missões", U"Máscaras", U"Cartões"},
                                    {"Japanese", U"ミニゲーム", U"ミッション", U"おめん", U"スタンプカード"},
                                    {"Korean", U"미니게임", U"미션", U"가면", U"스탬프 카드"},
                                    {"CN_Chinese", U"小游戏", U"任务", U"面具", U"集章卡"},
                                    {"TW_Chinese", U"小遊戲", U"任務", U"面具", U"集章卡"}};
        const Own* own = &Ours[1];
        for (const auto& o : Ours)
            if (dir.find(o.lang) != std::string::npos)
                own = &o;
        return section == 0 ? own->games : section == 1 ? own->missions : section == 2 ? own->masks : own->cards;
    }

    // Merry Magoland's missions (yaml/Scn/Yy/Cmn/MissionData): the sets in the game's own list order, each one's missions
    // in its list's order; the save keeps their phases (u32, 1 done) from 0x2B04 in this same order (validated on file 2)
    static constexpr std::array<std::string_view, KirbyMago::SetCount> MagoSets{"EggCatcher", "Setsuna", "Ochiochi", "Rally",
                                                                                "Punch",      "Ride",    "Hayauchi", "Shuriken",
                                                                                "Otsukai",    "Tps",     "Yy",       "Extra"};
    static constexpr int MagoMissionCount = 120, MagoRows = 20;
    // (the save's order is the game's YyMissionKind: its groups as YyMissionGroupKind has them, then by number; read by
    // StorageAccessor.GetMissionPhase, main+0x7BBA74: the phase at 0x2B0C + 4 * kind of the game's own view)
    static constexpr std::array<std::string_view, KirbyMago::SetCount> MagoGroups{"EggCatcher", "Hayauchi", "Ochiochi", "Otsukai",
                                                                                  "Punch",      "Rally",    "Ride",     "Setsuna",
                                                                                  "Shuriken",   "Tps",      "Yy",       "Extra"};
    struct MagoMission {
        std::string kind; // its texts' label: Title_<kind>, Desc_<kind>
        int difficulty{1};
        std::int64_t value{-1}; // Value or Count, the texts' numbers
        int seconds{-1};        // TimeLimitSec
        int save{-1};           // its place in the save
    };
    mutable std::array<std::vector<MagoMission>, KirbyMago::SetCount> mago_missions;
    mutable std::once_flag mago_missions_once;
    mutable std::atomic<bool> mago_missions_ok{false};

    void MagoMissionsLoad() const {
        std::call_once(mago_missions_once, [this] {
            const auto raw = romfs("/yaml/Scn/Yy/Cmn/MissionData.bin");
            const auto doc = raw ? KirbyAssets::ReadXbinYaml(*raw) : std::nullopt;
            int at = 0;
            for (std::size_t k = 0; doc && k < MagoSets.size(); ++k) {
                const auto* list = doc->Find(MagoSets[k]);
                if (!list || list->kind != KirbyAssets::YamlNode::Kind::List)
                    return;
                for (const auto& m : list->list) {
                    MagoMission mission;
                    if (const auto* v = m.Find("Kind"))
                        mission.kind = v->text;
                    if (const auto* v = m.Find("Difficulty"))
                        mission.difficulty = static_cast<int>(std::clamp<std::int64_t>(v->number, 1, 3));
                    if (const auto* v = m.Find("Value") ? m.Find("Value") : m.Find("Count"))
                        mission.value = v->number;
                    if (const auto* v = m.Find("TimeLimitSec"))
                        mission.seconds = static_cast<int>(v->number);
                    mago_missions[k].push_back(std::move(mission));
                }
                at += static_cast<int>(mago_missions[k].size());
            }
            // every kind's place: its group's, then its number's among the group's
            std::vector<std::tuple<int, int, MagoMission*>> kinds;
            for (auto& set : mago_missions)
                for (auto& m : set) {
                    const auto cut = m.kind.find("Mission");
                    const auto group = cut == std::string::npos ? MagoGroups.end() : std::ranges::find(MagoGroups, m.kind.substr(0, cut));
                    if (group == MagoGroups.end())
                        return;
                    kinds.emplace_back(static_cast<int>(group - MagoGroups.begin()), std::atoi(m.kind.c_str() + cut + 7), &m);
                }
            std::ranges::sort(kinds, [](const auto& a, const auto& b) { return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b)); });
            for (std::size_t i = 0; i < kinds.size(); ++i)
                std::get<2>(kinds[i])->save = static_cast<int>(i);
            mago_missions_ok = at == MagoMissionCount && kinds.size() == MagoMissionCount;
        });
    }

    // a save index's set and place in it
    std::optional<std::pair<int, int>> MagoMissionAt(int save) const {
        for (int k = 0; k < KirbyMago::SetCount; ++k)
            for (int i = 0; i < static_cast<int>(mago_missions[static_cast<std::size_t>(k)].size()); ++i)
                if (mago_missions[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)].save == save)
                    return std::pair{k, i};
        return std::nullopt;
    }

    // the missions done per set (the save's phases, > 0 done)
    std::array<int, KirbyMago::SetCount> MagoDone(const u32 (&phases)[120]) const {
        MagoMissionsLoad();
        std::array<int, KirbyMago::SetCount> done{};
        if (mago_missions_ok)
            for (std::size_t k = 0; k < done.size(); ++k)
                for (const auto& m : mago_missions[k])
                    done[k] += phases[m.save] > 0;
        return done;
    }

    // a mission text with the game's numbers in: 2.45 the mission's value, 2.46 the same in thousands, 2.26 its time limit
    std::u32string MagoText(const std::string& dir, const std::string& file, const std::string& label, const MagoMission* m) {
        const auto msbt = romfs("/msg/Ark/" + dir + "/" + file + ".msbt");
        if (!msbt)
            return {};
        const auto cmn = romfs("/msg/Ark/" + dir + "/YyCmn.msbt");
        const auto colon = cmn ? KirbyAssets::MsbtText(*cmn, "TimeDisplaySecPunctuation") : std::nullopt;
        const auto digits = [](std::int64_t v, char32_t group) {
            std::u32string out;
            const std::string d = std::to_string(std::max<std::int64_t>(0, v));
            for (std::size_t i = 0; i < d.size(); ++i) {
                if (group && i > 0 && (d.size() - i) % 3 == 0)
                    out += group;
                out += static_cast<char32_t>(d[i]);
            }
            return out;
        };
        // (thousands: a comma in English and the Asian languages, a space in French, a point elsewhere)
        const bool comma = dir.find("English") != std::string::npos || dir.find("Japanese") != std::string::npos ||
                           dir.find("Chinese") != std::string::npos || dir.find("Korean") != std::string::npos;
        const char32_t group = comma ? U',' : dir.find("French") != std::string::npos ? U' ' : U'.';
        const auto text = KirbyAssets::MsbtFilled(*msbt, label, [&](std::uint16_t g, std::uint16_t t) -> std::u32string {
            if (g != 2 || !m)
                return {};
            if (t == 45)
                return digits(m->value, 0);
            if (t == 46)
                return digits(m->value, group);
            if (t == 26 && m->seconds >= 0)
                return digits(m->seconds / 60, 0) + (colon && !colon->empty() ? *colon : U":") + (m->seconds % 60 < 10 ? U"0" : U"") +
                       digits(m->seconds % 60, 0);
            return {};
        });
        return text ? *text : std::u32string{};
    }

    std::u32string MagoSetName(const std::string& dir, int set) {
        if (set >= 10)
            return MagoText(dir, "YyMission", set == 10 ? "Yy" : "Extra", nullptr);
        return MagoText(dir, "Name/YySubGame", "HighScore_" + std::string{MagoSets[static_cast<std::size_t>(set)]}, nullptr);
    }

    // the Missions section's pictures: mago/mset/<dir>/<set>, mago/marrow/<0|1>, mago/mpins/<n>, mago/mdots/<sets>/<set>,
    // mago/mclear/<done>/<total>, mago/mrow/<dir>/<save>/<selected>/<done>, mago/mpin/<pinned>, mago/mtrack/<h>,
    // mago/mthumb/<h>, mago/mcard/<dir>/<save>/<done>
    std::optional<KirbyAssets::Image> RenderMagoMissions(const KirbyModes::Sources& src, const std::vector<std::string>& f) {
        const auto n = [&](std::size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
        if (f.size() == 4 && f[1] == "mset")
            return KirbyMago::SetBar(src, MagoSetName(f[2], n(3)));
        if (f.size() == 3 && f[1] == "marrow")
            return KirbyMago::SetArrow(src, f[2] == "1");
        if (f.size() == 3 && f[1] == "mpins")
            return KirbyMago::PinCount(src, n(2));
        if (f.size() == 4 && f[1] == "mdots")
            return KirbyMago::SetDots(src, n(2), n(3));
        if (f.size() == 4 && f[1] == "mclear")
            return KirbyMago::ClearCount(src, n(2), n(3));
        if (f.size() == 3 && f[1] == "mpin")
            return KirbyMago::Pin(src, f[2] == "1");
        if (f.size() == 2 && f[1] == "mtrack")
            return KirbyMago::ScrollTrack(src, static_cast<std::uint32_t>(MisView));
        if (f.size() == 2 && f[1] == "mthumb")
            return KirbyMago::ScrollThumb(src, KirbyMago::ThumbSize[1]);
        MagoMissionsLoad();
        const auto at = f.size() >= 5 && mago_missions_ok ? MagoMissionAt(n(3)) : std::nullopt;
        if (!at)
            return std::nullopt;
        const MagoMission& m = mago_missions[static_cast<std::size_t>(at->first)][static_cast<std::size_t>(at->second)];
        if (f.size() == 6 && f[1] == "mrow")
            return KirbyMago::MissionRow(src, MagoText(f[2], "YyMission", "Title_" + m.kind, &m), f[4] == "1", f[5] == "1");
        if (f.size() == 5 && f[1] == "mcard") {
            KirbyMago::CardText card;
            card.headline = MagoText(f[2], "YyCmn", "$ContentsHeadline", nullptr);
            card.criteria = MagoText(f[2], "YyMission", "Desc_" + m.kind, &m);
            card.comment = MagoText(f[2], "YyMission", "Difficulty0" + std::to_string(m.difficulty), nullptr);
            card.difficulty = m.difficulty, card.done = f[4] == "1";
            return KirbyMago::MissionCard(src, card);
        }
        return std::nullopt;
    }

    // the Masks section's buttons: mago/mask/r/<dir>/<worn> (Remove), mago/mask/q/<dir> (a mask not got),
    // mago/mask/i/<dir>/<kind>/<voice>/<worn>; the mark and the player's tag in the game's words (YyCmn)
    std::optional<KirbyAssets::Image> RenderMagoMask(const KirbyModes::Sources& src, const std::vector<std::string>& f) {
        const auto word = [&](const std::string& dir, const std::string& label, const char32_t* fallback) {
            const auto m = romfs("/msg/Ark/" + dir + "/YyCmn.msbt");
            const auto t = m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
            return t && !t->empty() ? *t : std::u32string{fallback};
        };
        const auto player = [&](const std::string& dir, const std::string& worn) {
            return worn == "1" ? word(dir, "OmenSelectPlayerNum_01", U"P1") : std::u32string{};
        };
        if (f.size() == 5 && f[2] == "r")
            return KirbyMago::MaskRemove(src, player(f[3], f[4]));
        if (f.size() == 4 && f[2] == "q")
            return KirbyMago::MaskLocked(src, word(f[3], "$OmenSelectNone", U"?"));
        const int kind = f.size() == 7 && f[2] == "i" ? std::atoi(f[4].c_str()) : 0;
        if (kind < 1 || kind >= KirbyMago::MaskSlots)
            return std::nullopt;
        return KirbyMago::MaskButton(src, std::string{KirbyMago::MaskNames[static_cast<std::size_t>(kind - 1)]}, f[5] == "1", player(f[3], f[6]));
    }

    // the game page's own words (the game has none for them): the pinned missions' box, its line with no pin, the tour's
    // line with no tour (the tour box's name is the game's: YmGrandPrix's Enter_Title)
    std::u32string GameWord(const std::string& dir, int which) const {
        struct Own {
            const char* lang;
            const char32_t *pinned, *no_pins, *no_tour;
        };
        static constexpr Own Ours[]{{"Italian", U"Missioni Fissate", U"Non ci sono missioni fissate.", U"Non ci sono dei Tour in corso."},
                                    {"English", U"Pinned Missions", U"There are no pinned missions.", U"No Magoland Tour is under way."},
                                    {"French", U"Missions épinglées", U"Aucune mission épinglée.", U"Aucun tour de Magoland en cours."},
                                    {"German", U"Angeheftete Missionen", U"Keine angehefteten Missionen.", U"Keine Magoland-Tour im Gange."},
                                    {"Spanish", U"Misiones fijadas", U"No hay misiones fijadas.", U"No hay ningún tour en curso."},
                                    {"Dutch", U"Vastgezette missies", U"Er zijn geen vastgezette missies.", U"Er is geen Magoland-tour bezig."},
                                    {"Portuguese", U"Missões fixadas", U"Não há missões fixadas.", U"Não há nenhum tour em andamento."},
                                    {"Japanese", U"ピンどめミッション", U"ピンどめしたミッションはありません", U"ツアーは おこなわれていません"},
                                    {"Korean", U"고정한 미션", U"고정한 미션이 없습니다.", U"진행 중인 투어가 없습니다."},
                                    {"CN_Chinese", U"固定的任务", U"没有固定的任务。", U"没有进行中的巡游。"},
                                    {"TW_Chinese", U"固定的任務", U"沒有固定的任務。", U"沒有進行中的巡遊。"}};
        const Own* own = &Ours[1];
        for (const auto& o : Ours)
            if (dir.find(o.lang) != std::string::npos)
                own = &o;
        return which == 0 ? own->pinned : which == 1 ? own->no_pins : own->no_tour;
    }

    // the game page's pictures: game/ticket/<msg dir>/<kind>, game/box/<msg dir>/<0 pins|1 tour>/<empty>,
    // game/tcard/<msg dir>/<kind>, game/tarrow, game/tcursor (kinds: ScnContext.Yy.YySubGameKind's, GameKinds)
    std::optional<KirbyAssets::Image> RenderGame(const std::vector<std::string>& f) {
        const auto src = ModesSources();
        const int kind = f.size() == 4 ? std::atoi(f[3].c_str()) : -1;
        const auto text = [&](const std::string& dir, const std::string& file, const std::string& label) {
            const auto m = romfs("/msg/Ark/" + dir + "/" + file + ".msbt");
            const auto t = m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
            return t ? *t : std::u32string{};
        };
        if (f.size() == 4 && f[1] == "ticket" && kind >= 0 && kind < static_cast<int>(GameKinds.size()))
            return KirbyMago::GameTicket(src, kind, text(f[2], "Name/YySubGame", std::string{GameKinds[static_cast<std::size_t>(kind)]}));
        if (f.size() == 4 && f[1] == "tcard" && kind >= 0 && kind < 10)
            return KirbyMago::TourCard(src, kind, text(f[2], "Name/YySubGame", "GrandPrixThumbnail_" + std::string{GameKinds[static_cast<std::size_t>(kind)]}));
        if (f.size() == 5 && f[1] == "box") {
            const bool tour = f[3] == "1";
            std::u32string name = tour ? text(f[2], "YmGrandPrix", "Enter_Title") : GameWord(f[2], 0);
            if (name.empty())
                name = U"Magoland Tour";
            return KirbyMago::GameBox(src, tour ? KirbyMago::TourBoxSize : KirbyMago::PinsBoxSize, tour, name,
                                      f[4] == "1" ? GameWord(f[2], tour ? 2 : 1) : std::u32string{});
        }
        if (f.size() == 2 && f[1] == "tarrow")
            return KirbyMago::TourArrow(src);
        if (f.size() == 2 && f[1] == "tcursor")
            return KirbyMago::TourCursor(src);
        return std::nullopt;
    }

    // mago/bg, mago/arrow/<0|1>, mago/title/<msg dir>/<section>, mago/dots/<section>, mago/count/<msg dir>/<done>/<total>,
    // mago/door/<0..9> (GateDecoModel's order), mago/globe: Merry Magoland's plaza page (KirbyMago, KirbyGates)
    std::optional<KirbyAssets::Image> RenderMago(const std::vector<std::string>& f) {
        const auto src = ModesSources();
        const auto model = [&](std::string_view file) {
            const auto raw = romfs("/gfx/" + std::string{file} + ".bfres.cmp");
            return raw ? KirbyAssets::DecompressCmp(*raw) : std::nullopt;
        };
        const auto textures = [&](std::string_view file) {
            return KirbyGates::Textures{[this, file](const std::string& n) { return assets.Load(romfs, "gfx/" + std::string{file} + "/" + n); }};
        };
        if (f.size() == 2 && f[1] == "bg")
            return KirbyMago::Background(src);
        if (f.size() == 3 && f[1] == "arrow")
            return KirbyMago::Arrow(src, f[2] == "1");
        if (f.size() == 3 && f[1] == "dots")
            return KirbyMago::Dots(src, std::atoi(f[2].c_str()));
        if (f.size() == 4 && f[1] == "title") {
            std::array<std::u32string, KirbyMago::Sections> names;
            for (int i = 0; i < KirbyMago::Sections; ++i)
                names[static_cast<std::size_t>(i)] = MagoName(f[2], i);
            return KirbyMago::Title(src, MagoName(f[2], std::atoi(f[3].c_str())), names);
        }
        if (f.size() == 5 && f[1] == "count") {
            const auto m = romfs("/msg/Ark/" + f[2] + "/YyCmn.msbt");
            const auto label = m ? KirbyAssets::MsbtText(*m, "$MissionText") : std::nullopt;
            return KirbyMago::Missions(src, label && !label->empty() ? *label : U"Missions", std::atoi(f[3].c_str()), std::atoi(f[4].c_str()));
        }
        if (f.size() == 3 && f[1] == "door") {
            const auto gate = model(KirbyGates::GateFile), decos = model(KirbyGates::DecoFile);
            if (!gate || !decos)
                return std::nullopt;
            return KirbyGates::Door(*gate, textures(KirbyGates::GateFile), *decos, textures(KirbyGates::DecoFile), std::atoi(f[2].c_str()), 3);
        }
        if (f.size() == 2 && f[1] == "globe") {
            const auto m = model(KirbyGates::GlobeFile);
            return m ? KirbyGates::Globe(*m, textures(KirbyGates::GlobeFile), 3) : std::nullopt;
        }
        if (f.size() >= 4 && f[1] == "mask")
            return RenderMagoMask(src, f);
        // mago/scard/<msg dir>/<card>/<past the 99th>/<stamps>: the card and its number ("No. %02d" in the game's words);
        // mago/sbar/<stamps got>/<cards finished>
        if (f.size() == 6 && f[1] == "scard") {
            const int card = std::clamp(std::atoi(f[3].c_str()), 1, 99);
            auto look = KirbyMago::StampRules(card, std::atoi(f[4].c_str()), std::atoi(f[5].c_str()));
            const std::u32string digits{static_cast<char32_t>(U'0' + card / 10), static_cast<char32_t>(U'0' + card % 10)};
            const auto m = romfs("/msg/Ark/" + f[2] + "/YmEntrance.msbt");
            const auto text = m ? KirbyAssets::MsbtFilled(*m, "StampRally_CardNo", [&](std::uint16_t g, std::uint16_t t) {
                return g == 2 && t == 24 ? digits : std::u32string{};
            })
                                : std::nullopt;
            look.number = text && !text->empty() ? *text : U"No. " + digits;
            return KirbyMago::StampCard(src, look);
        }
        if (f.size() == 4 && f[1] == "sbar")
            return KirbyMago::StampBar(src, std::atoi(f[2].c_str()), std::atoi(f[3].c_str()));
        return f.size() >= 2 && f[1].starts_with('m') ? RenderMagoMissions(src, f) : std::nullopt;
    }

    std::optional<KirbyAssets::Image> RenderPlanet(KirbyPlanet::Planet p) {
        const auto raw = romfs(KirbyPlanet::ModelPath(p));
        const auto bfres = raw ? KirbyAssets::DecompressCmp(*raw) : std::nullopt;
        if (!bfres)
            return std::nullopt;
        const KirbyPlanet::Model model{p, *bfres};
        const std::string dir = p == KirbyPlanet::Planet::PopStar ? "gfx/WMap/Map/Popstar/Bg/" : "gfx/WMap/Map/Halcandora/Bg/";
        return model.Draw([&](const std::string& n) { return assets.Load(romfs, dir + n); }, 1000, 2);
    }

    // wmap/<msg dir>/<planet 0|1>/<mode 0|1>/<lives>.<stars>.<spheres>
    std::optional<KirbyAssets::Image> RenderWorldMap(const std::vector<std::string>& f) {
        const bool halcandra = f[2] == "1";
        const auto sky = FilesImage("wsky");
        const auto planet = FilesImage(halcandra ? "planet/1" : "planet/0");
        const auto counts = Numbers(f[4]);
        const auto msbt = romfs("/msg/Ark/" + f[1] + "/WorldMap.msbt");
        const auto name = msbt ? KirbyAssets::MsbtText(*msbt, halcandra ? "WorldName_Halcandra" : "WorldName_PopStar") : std::nullopt;
        if (!sky || !planet || counts.size() != 3 || !name)
            return std::nullopt;
        return KirbyModes::WorldMapPage(ModesSources(), *sky, *planet, {halcandra, f[3] == "1", counts[0], counts[1], counts[2], *name});
    }

    static std::vector<int> Numbers(const std::string& field) {
        std::vector<int> v;
        for (std::size_t a = 0, b; a <= field.size(); a = b + 1) {
            b = std::min(field.find('.', a), field.size());
            v.push_back(std::atoi(field.substr(a, b - a).c_str()));
        }
        return v;
    }

    std::optional<KirbyAssets::Image> RenderEpilogue(const std::vector<std::string>& f) {
        static constexpr int Max[KirbyModes::Techniques]{9, 6, 6, 6, 6, 3, 3, 3, 3, 6, 9, 9};
        static constexpr int Trial[KirbyModes::Areas][3]{{0, 9, 10}, {3, 2, 1}, {4, 5, 8}, {6, 7, 11}};
        const auto head = Numbers(f[2]), levels = Numbers(f[3]);
        const auto msg = romfs("/msg/Ark/" + f[1] + "/Mahoroa.msbt");
        if (head.size() != 3 || levels.size() != KirbyModes::Techniques || !msg)
            return std::nullopt;
        KirbyModes::EpiloguePage e;
        e.magic = head[0], e.sprout = head[1], e.open = head[2];
        for (int t = 0; t < KirbyModes::Techniques; ++t)
            e.level[t] = levels[t], e.max[t] = Max[t];
        for (int a = 0; a < KirbyModes::Areas; ++a) {
            const auto v = Numbers(f[4 + a]);
            if (v.size() < 4)
                continue;
            auto& area = e.areas[a];
            const auto text = [&](const std::string& label) {
                const auto t = KirbyAssets::MsbtText(*msg, label);
                return t ? *t : std::u32string{};
            };
            area.number = text("Area" + std::to_string(a + 1) + "Number");
            area.name = text("Area" + std::to_string(a + 1) + "Name");
            area.boss = v[0];
            for (int k = 0; k < 3; ++k)
                area.trial_state[k] = v[1 + k], area.trials[k] = Trial[a][k];
            area.medals.assign(v.begin() + 4, v.end());
        }
        return KirbyModes::EpiloguePanel(ModesSources(), e);
    }

    // mbtn/<msg dir>/<cursor id>: the button with its TitleMenu name
    std::optional<KirbyAssets::Image> RenderMenuButton(const std::vector<std::string>& f) {
        const int b = std::atoi(f[2].c_str());
        const auto msg = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
        const char* label = b == 3 ? "ModeSelectTheaterRoom" : b == 4 ? "ModeSelectSoundRoom" : b == 5 ? "PrimaryModeSelectWaiwai"
                                                                                                    : "ModeSelectHistory";
        const auto text = msg ? KirbyAssets::MsbtText(*msg, label) : std::nullopt;
        return text ? KirbyModes::MenuButton(ModesSources(), b, *text) : std::nullopt;
    }

    std::optional<KirbyAssets::Image> RenderMenuTitle(const std::vector<std::string>& f) {
        const auto msg = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
        const auto text = msg ? KirbyAssets::MsbtText(*msg, "ModeSelectTitle") : std::nullopt;
        return text ? KirbyModes::MenuTitle(ModesSources(), *text) : std::nullopt;
    }

    std::optional<KirbyAssets::Image> RenderArena(const std::vector<std::string>& f) {
        const auto msg = romfs("/msg/Ark/" + f[1] + "/Arena.msbt");
        if (!msg)
            return std::nullopt;
        const auto text = [&](const char* label) {
            const auto t = KirbyAssets::MsbtText(*msg, label);
            return t ? *t : std::u32string{};
        };
        KirbyModes::ArenaPage a;
        a.final = f[2] == "1";
        a.cleared = f[3] == "1";
        a.rounds = a.final ? 21 : 13;
        a.alone = text("RankingPersonal"), a.together = text("RankingFriend");
        a.ok = text("ResultClear"), a.none = text("RankingTimeNoRecord");
        // "Round {2.1}": the text before its number
        a.round = text("RankingRound");
        for (int r = 0; r < 6; ++r) {
            const auto v = Numbers(f[4 + r]);
            if (v.size() != 8)
                return std::nullopt;
            a.records[r] = {v[0], v[1], {v[2], v[3], v[4], v[5]}, v[6] != 0, v[7] != 0};
        }
        return KirbyModes::ArenaPanel(ModesSources(), a);
    }

    // The Arenas' names: TitleMenu's unlock notices name them in their first coloured run
    std::optional<KirbyAssets::Image> RenderArenaTitle(const std::vector<std::string>& f) {
        const bool final = f[2] == "4";
        const auto msg = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
        const auto runs = msg ? KirbyAssets::MsbtRuns(*msg, final ? "UnlockArenaEX" : "UnlockArena") : std::nullopt;
        if (runs)
            for (const auto& r : *runs)
                if (r.colour != 0)
                    return KirbyModes::ArenaTitle(ModesSources(), final, r.text);
        return std::nullopt;
    }

    // The Epilogue's name: TitleMenu's notice for its mask names it in its first coloured run
    std::optional<KirbyAssets::Image> RenderEpilogueTitle(const std::vector<std::string>& f) {
        const auto msg = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
        const auto runs = msg ? KirbyAssets::MsbtRuns(*msg, "DialogNewOmenNotifyDesc_MahoraWhite") : std::nullopt;
        if (!runs)
            return std::nullopt;
        for (const auto& r : *runs)
            if (r.colour != 0)
                return KirbyModes::EpilogueTitle(ModesSources(), r.text);
        return std::nullopt;
    }

    // advt/<msg dir>/<mode>
    std::optional<KirbyAssets::Image> RenderAdventureTitle(const std::vector<std::string>& f) {
        const bool extra = f[2] == "1";
        const auto msg = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
        const auto title = msg ? KirbyAssets::MsbtText(*msg, extra ? "ModeSelectStoryExtra" : "ModeSelectStory") : std::nullopt;
        return title ? KirbyModes::AdventureTitle(ModesSources(), extra, *title) : std::nullopt;
    }

    static std::u32string Ascii(std::string_view s) {
        return {s.begin(), s.end()};
    }

    // Keys after gen/files/:
    //   panel/<n|c|d>/<slot>[/<scale>]   rainbow/<step>   cursor   dim   flash   kirby/<face>/<crown>
    //   label/<msg dir>/<number>   pct/<digits|->   modes/<look>/<story grey><magoland grey><lock>
    //   modepct/<look>/<digits|->/<digits|->
    //   mode/<msg dir>/<story>/<extra>/<epilogue>/<magoland>   level/<Magolor's level>   max/<rainbow step>   mainmenu
    //   advp/<msg dir>/<mode>/<counts>/<spheres got>/<spheres of>/<bosses beaten>/<medals>   advt/<msg dir>/<mode>   magolor
    //   curtain
    std::optional<KirbyAssets::Image> RenderFiles(std::string_view key) {
        std::vector<std::string> f;
        for (std::size_t a = 0, b; a <= key.size(); a = b + 1) {
            b = std::min(key.find('/', a), key.size());
            f.emplace_back(key.substr(a, b - a));
        }
        const auto look = [&](const std::string& c) {
            return c == "c" ? KirbyFiles::Look::Complete : c == "d" ? KirbyFiles::Look::NoData : KirbyFiles::Look::Normal;
        };
        const auto value = [](const std::string& v) { return v == "-" ? std::u32string{} : Ascii(v); };
        if (f[0] == "fx")
            return RenderFx(f);
        if (f[0].starts_with("soul"))
            return RenderSoul(f);
        if (f[0] == "mode" && f.size() == 6)
            return RenderModes(f);
        if (f[0] == "advp" && f.size() == 8)
            return RenderAdventure(f);
        if (f[0] == "advt" && f.size() == 3)
            return f[2] == "2"                  ? RenderEpilogueTitle(f)
                   : f[2] == "3" || f[2] == "4" ? RenderArenaTitle(f)
                   : f[2] == "5"                ? RenderMenuTitle(f)
                                                : RenderAdventureTitle(f);
        if (f[0] == "epi" && f.size() == 8)
            return RenderEpilogue(f);
        if (f[0] == "arena" && f.size() == 10)
            return RenderArena(f);
        if (f[0] == "mbtn" && f.size() == 3)
            return RenderMenuButton(f);
        if (f[0] == "mcur" && f.size() == 2)
            return KirbyModes::MenuButtonCursor(ModesSources(), std::atoi(f[1].c_str()));
        if (f[0] == "mainmenu")
            return MainMenu();
        if (f[0] == "curtain")
            return KirbyModes::CurtainPage(ModesSources());
        if (f[0] == "mago")
            return RenderMago(f);
        if (f[0] == "game")
            return RenderGame(f);
        if (f[0] == "wmap" && f.size() == 5)
            return RenderWorldMap(f);
        // the title page's pictures and the stage background's mask, drawn by the workers like the rest so
        // the host never makes them on its own loading thread
        if (f[0] == "titlebg") {
            const auto page = TitlePage();
            return page ? std::optional<KirbyAssets::Image>{*page} : std::nullopt;
        }
        if (f[0] == "logo" && f.size() == 2)
            return TitleLogoImage(f[1], 400, 290);
        if (f[0] == "startbtn" && f.size() == 2)
            return StartButtonImage(f[1]);
        if (f[0] == "lvicon" && f.size() == 2) {
            const auto icon = assets.Load(romfs, "lyt/Resident/LevelBoard/Body/LvMapIcon" + std::string{f[1]} + "^u");
            return icon ? std::optional<KirbyAssets::Image>{*icon} : std::nullopt;
        }
        if (f[0] == "bgmask") {
            const auto mask = assets.Load(romfs, "lyt/Pause/Parts/PauseBg^t");
            if (!mask)
                return std::nullopt;
            KirbyAssets::Image out = *mask;
            for (std::size_t i = 0; i < out.rgba.size(); i += 4)
                out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 0xFF;
            return out;
        }
        if (f[0] == "bggear")
            return KirbyModes::BackgroundGear(ModesSources());
        if (f[0] == "wcur" && f.size() == 2)
            return KirbyModes::WorldCursor(static_cast<float>(std::atoi(f[1].c_str())) / 10);
        if (f[0] == "epibar" && f.size() == 6) {
            const int level = std::atoi(f[2].c_str()), missing = std::atoi(f[3].c_str());
            const auto words = EpiTexts(f[1]);
            const auto num = [](int v) {
                const auto d = std::to_string(v);
                return std::u32string(d.begin(), d.end());
            };
            const std::u32string text = level >= EpiLevels ? words.level + U" " + words.max
                                        : words.level + U" " + num(level) + U" | -" + num(missing) + U" " +
                                              words.to + U" " + words.level + U" " + num(level + 1);
            const bool charging = f[5].ends_with("c");
            return KirbyModes::EpilogueBar(ModesSources(), {text, std::atoi(f[4].c_str()), std::atoi(f[5].c_str()), charging});
        }
        if (f[0] == "epirbtn" && f.size() == 3) {
            const auto texts = PlayTexts(f[1]);
            const bool pause = f[2] == "pause";
            return KirbyModes::RoundButton(ModesSources(), pause ? KirbyModes::RoundIcon::MagolorPause : KirbyModes::RoundIcon::MagolorContinue,
                                           pause ? texts.pause : texts.resume, KirbyModes::EpiRoundSize);
        }
        if (f[0] == "episp" && f.size() == 4) {
            const int which = std::clamp(std::atoi(f[2].c_str()), 0, 3);
            const auto state = static_cast<KirbyModes::SpecialState>(std::clamp(std::atoi(f[3].c_str()), 0, 2));
            const auto words = EpiTexts(f[1]);
            return KirbyModes::SpecialButton(ModesSources(), which, words.moves[static_cast<std::size_t>(which)], KirbyModes::SpecialSize, state);
        }
        // a magic's popup: 0 Heal, 1 Magolor Blast
        if (f[0] == "epipop" && f.size() == 3) {
            const auto words = EpiTexts(f[1]);
            const std::size_t k = f[2] == "1" ? 1 : 0;
            const auto icon = KirbyModes::SpecialButton(ModesSources(), static_cast<int>(2 + k), words.moves[2 + k], 250, KirbyModes::SpecialState::Ready);
            return icon ? KirbyModes::MagicPopup(ModesSources(), words.learnt[k], words.info[k], *icon) : std::nullopt;
        }
        if (f[0] == "epipopbtn" && f.size() == 2)
            return KirbyModes::PopupButton(ModesSources(), EpiTexts(f[1]).ok);
        // the colours a goal unlocked: spraypop/<msg dir>/<spheres>/<preset>.<preset>..., and its button
        if (f[0] == "spraypop" && f.size() == 4) {
            std::vector<KirbyAssets::Image> cans;
            std::vector<std::u32string> names;
            const auto colours = Numbers(f[3]);
            for (const int i : colours)
                if (i >= 0 && i < static_cast<int>(KirbyColour::Presets.size())) {
                    const auto& p = KirbyColour::Presets[static_cast<std::size_t>(i)];
                    // (smaller past three, to fit the panel)
                    cans.push_back(KirbyColour::Spray(p, colours.size() <= 3 ? 8 : 6));
                    names.emplace_back(KirbyColour::NameIn(p, f[1], romfs));
                }
            if (names.empty())
                return std::nullopt;
            const auto [title, message] = KirbyModes::SprayWords(f[1], std::atoi(f[2].c_str()), names);
            return KirbyModes::SprayPopup(ModesSources(), title, message, cans);
        }
        if (f[0] == "spraybtn" && f.size() == 2)
            return KirbyModes::SprayButton(ModesSources(), EpiTexts(f[1]).ok);
        if (f[0] == "epitab" && f.size() == 3) {
            const int i = std::clamp(std::atoi(f[2].c_str()), 0, 2);
            const auto texts = PlayTexts(f[1]);
            static constexpr int Colours[]{3, 4, 2};
            return KirbyModes::TabButton(ModesSources(), Colours[i], i == 0 ? texts.game : i == 1 ? EpiTexts(f[1]).magic : texts.options,
                                         KirbyModes::TabSize[0], KirbyModes::TabSize[1]);
        }
        if (f[0] == "epiaura" && f.size() == 2)
            return KirbyModes::GaugeAura(std::clamp(std::atoi(f[1].c_str()), 0, 2));
        if (f[0] == "efin")
            return RenderFinale(f);
        if (f[0] == "epigauge" && f.size() >= 2) {
            static const std::map<std::string, KirbyModes::GaugePart> Parts{
                {"charging", KirbyModes::GaugePart::Charging}, {"full", KirbyModes::GaugePart::Full}, {"rainbow", KirbyModes::GaugePart::Rainbow},
                {"particles1", KirbyModes::GaugePart::Particles1}, {"particles2", KirbyModes::GaugePart::Particles2},
                {"glow", KirbyModes::GaugePart::Glow}, {"empty", KirbyModes::GaugePart::Empty}, {"edge", KirbyModes::GaugePart::Edge},
                {"front", KirbyModes::GaugePart::Front}, {"locked", KirbyModes::GaugePart::Locked}};
            const auto part = Parts.find(std::string{f[1]});
            if (part == Parts.end())
                return std::nullopt;
            return KirbyModes::GaugePiece(ModesSources(), part->second, f.size() == 3 ? std::atoi(f[2].c_str()) : 0);
        }
        if (f[0] == "play" && f.size() == 7) {
            const auto counts = Numbers(f[3]), spheres = Numbers(f[5]), boss = Numbers(f[6]);
            if (counts.size() != 2 || spheres.size() != 2 || boss.size() != 2)
                return std::nullopt;
            return KirbyModes::PlayBarPicture(ModesSources(), {f[2] == "1", counts[0], counts[1], std::atoi(f[4].c_str()),
                                                                f[4].ends_with('c'), spheres[0], static_cast<unsigned>(spheres[1]),
                                                                boss[0], boss[1] == 1});
        }
        if (f[0] == "ablinfo" && f.size() == 3) {
            const auto content = AbilityOf(f[1], std::atoi(f[2].c_str()));
            const auto icons = KirbyFont::LoadIconFont(romfs, "/font/Region/STD/ExtOperation.bffnt.cmp");
            return content ? KirbyModes::AbilityInfo(ModesSources(), icons.get(), content->info, content->picture) : std::nullopt;
        }
        if (f[0] == "ablpage" && f.size() == 4) {
            const auto content = AbilityOf(f[1], std::atoi(f[2].c_str()));
            const auto icons = KirbyFont::LoadIconFont(romfs, "/font/Region/STD/ExtOperation.bffnt.cmp");
            const auto page = static_cast<std::size_t>(std::atoi(f[3].c_str()));
            if (!content)
                return std::nullopt;
            return KirbyModes::AbilityMoves(ModesSources(), icons.get(),
                                            page < content->pages.size() ? content->pages[page] : std::vector<KirbyModes::MoveRow>{});
        }
        if (f[0].starts_with("opt") && f.size() >= 2) {
            const auto w = KirbyModes::OptionTexts(f[1]);
            const auto src = ModesSources();
            const int a = f.size() > 2 ? std::atoi(f[2].c_str()) : 0, b = f.size() > 3 ? std::atoi(f[3].c_str()) : 0;
            if (f[0] == "optrow" && f.size() == 4)
                return KirbyModes::OptionRow(src, a == 0 ? w.background : a == 1 ? w.hud : w.cheats, a == 0 ? (b ? w.no : w.yes) : U"");
            if (f[0] == "optpanel" && f.size() == 3)
                return KirbyModes::OptionPanel(src, a == 1 ? w.hud : w.cheats, a == 2 ? w.note : U"");
            if (f[0] == "optline" && f.size() == 4) {
                // the stage's counters, its cheat's label, the Epilogue's counters and its cheats' labels
                const std::u32string labels[]{w.counters[0], w.counters[1], w.counters[2], w.counters[3], w.ability,
                                              w.magic,       w.energy,      w.fill,        w.edit};
                if (a < 0 || a >= static_cast<int>(std::size(labels)))
                    return std::nullopt;
                return KirbyModes::OptionLine(src, labels[a], a == 4 || a >= 7 ? U"" : b ? w.hide : w.show);
            }
            if (f[0] == "optapply")
                return KirbyModes::CheatApply(src, w.apply, f.size() == 3 && f[2] == "off");
            if (f[0] == "optcard" && f.size() == 3) {
                const auto key = KirbyModes::AbilityKey(a);
                const auto pause = romfs("/msg/Ark/" + f[1] + "/Pause.msbt");
                const auto name = pause ? KirbyAssets::MsbtText(*pause, "Abl" + key + "Title") : std::nullopt;
                const auto star = AbilityStar(key);
                return KirbyModes::CheatCard(src, star.icon, name.value_or(U""), star.black, star.white);
            }
            return std::nullopt;
        }
        // the Epilogue's cheats page (the light rule between its two cheats) and its magic points' amount
        if (f[0] == "epopanel" && f.size() == 3) {
            return KirbyModes::OptionPanel(ModesSources(), KirbyModes::OptionTexts(f[1]).cheats, U"", EpiCheatRules);
        }
        if (f[0] == "epoapply" && f.size() == 3)
            return KirbyModes::CheatApply(ModesSources(), KirbyModes::OptionTexts(f[1]).apply, f[2] == "1", KirbyModes::EpiApplySize);
        if (f[0] == "epocard" && f.size() == 2)
            return KirbyModes::MagicPointsCard(ModesSources(), std::atoi(f[1].c_str()));
        // the Arenas: arenabar/<dir>/<ability>/<charge[c]>, the figures, the name plates
        // (arenaplate/<dir>/0/<character> from Arena.msbt MemberSelectChara<character>, /1/<boss> from Step.msbt Chara<boss>)
        if (f[0] == "arenabar" && f.size() == 4) {
            const auto star = AbilityStar(KirbyModes::AbilityKey(std::atoi(f[2].c_str())));
            return KirbyModes::ArenaBar(ModesSources(), {star.icon, star.black, star.white, std::atoi(f[3].c_str()), f[3].ends_with('c')});
        }
        if (f[0] == "arenakirby" && f.size() == 3)
            return RenderArenaKirby(f[1], std::atoi(f[2].c_str()));
        if (f[0] == "arenaboss" && f.size() == 3)
            return RenderArenaBoss(f[1], std::atoi(f[2].c_str()));
        // the VS scene's pictures: vsburst/<style>, vssparks/<0 player|1 boss|2 burst>/<frame>
        if (f[0] == "vsburst" && f.size() == 2)
            return KirbyModes::VsBurst(ModesSources(), std::atoi(f[1].c_str()), KirbyModes::VsSize);
        if (f[0] == "vssparks" && f.size() == 3) {
            const int which = std::clamp(std::atoi(f[1].c_str()), 0, 2);
            const auto size = which < 2 ? std::array<std::uint32_t, 2>{static_cast<std::uint32_t>(KirbyModes::VsSparksBox[which][2]),
                                                                        static_cast<std::uint32_t>(KirbyModes::VsSparksBox[which][3])}
                                        : std::array<std::uint32_t, 2>{KirbyModes::VsSize, KirbyModes::VsSize};
            return KirbyModes::VsSparks(which, std::atoi(f[2].c_str()), size[0], size[1]);
        }
        // arenapanel/<0 player|1 boss>: the ground behind a figure
        if (f[0] == "arenapanel" && f.size() == 2) {
            const bool boss = f[1] == "1";
            const auto size = boss ? KirbyModes::BossPanelSize : KirbyModes::PlayerPanelSize;
            return KirbyModes::FigurePanel(ModesSources(), boss ? KirbyModes::Plate::Boss : KirbyModes::Plate::Player, size[0], size[1]);
        }
        // (Kirby's plate may end in /<skin><name ink> of his spray: PlateToken)
        if (f[0] == "arenaplate" && (f.size() == 4 || (f.size() == 5 && f[4].size() == 12))) {
            const bool boss = f[2] == "1";
            f[3].resize(std::min(f[3].find('-'), f[3].size()));
            const auto msbt = romfs("/msg/Ark/" + f[1] + (boss ? "/Step.msbt" : "/Arena.msbt"));
            auto name = msbt ? KirbyAssets::MsbtText(*msbt, (boss ? "Chara" : "MemberSelectChara") + f[3]) : std::nullopt;
            // the groups' own names (the True Arena's with an X, as the game names its rounds)
            if (const auto* g = boss ? GroupOf(f[3]) : nullptr; g && !g->name.empty())
                name = std::u32string{g->name} + (f[3].starts_with("Ex") ? U" X" : U"");
            // a pair or a group without a name of its own: its bosses' names
            if (!name && msbt && boss)
                for (std::size_t a = 0, b; a < f[3].size(); a = b + 1) {
                    b = std::min(f[3].find('_', a), f[3].size());
                    if (const auto one = KirbyAssets::MsbtText(*msbt, "Chara" + f[3].substr(a, b - a)))
                        name = name ? *name + U" & " + *one : *one;
                }
            if (!name)
                return std::nullopt;
            std::replace(name->begin(), name->end(), U'\n', U' ');
            std::optional<std::array<std::uint32_t, 2>> tint;
            if (f.size() == 5)
                tint = {static_cast<std::uint32_t>(std::strtoul(f[4].substr(0, 6).c_str(), nullptr, 16)),
                        static_cast<std::uint32_t>(std::strtoul(f[4].substr(6).c_str(), nullptr, 16))};
            return KirbyModes::NamePlate(ModesSources(), boss ? KirbyModes::Plate::Boss : KirbyModes::Plate::Player, *name, boss ? "" : f[3], tint);
        }
        if (f[0] == "ablpill" && f.size() == 3)
            return KirbyModes::AbilityPill(ModesSources(), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()));
        if (f[0] == "ablarrow" && f.size() == 3)
            return KirbyModes::AbilityArrow(ModesSources(), f[1] == "1", f[2] == "1");
        if (f[0] == "dblarrow" && f.size() == 2)
            return KirbyModes::DoubleArrow(ModesSources(), f[1] == "1");
        // the Epilogue's Magic tab: epiminfo/<dir>/<restored>, epimpage/<dir>/<moves>/<page>, its counter and arrows
        if (f[0] == "epiminfo" && f.size() == 3) {
            const auto content = MagolorOf(f[1], "", f[2] == "1");
            const auto icons = KirbyFont::LoadIconFont(romfs, "/font/Region/STD/ExtOperation.bffnt.cmp");
            return content ? KirbyModes::AbilityInfo(ModesSources(), icons.get(), content->info, content->picture, KirbyModes::PauseLook::Magolor)
                           : std::nullopt;
        }
        if (f[0] == "epimpage" && f.size() == 4) {
            const auto content = MagolorOf(f[1], f[2], false);
            const auto icons = KirbyFont::LoadIconFont(romfs, "/font/Region/STD/ExtOperation.bffnt.cmp");
            const auto page = static_cast<std::size_t>(std::atoi(f[3].c_str()));
            if (!content)
                return std::nullopt;
            return KirbyModes::AbilityMoves(ModesSources(), icons.get(),
                                            page < content->pages.size() ? content->pages[page] : std::vector<KirbyModes::MoveRow>{},
                                            KirbyModes::AbilityMovesSize, KirbyModes::PauseLook::Magolor);
        }
        if (f[0] == "epimtech" && f.size() == 3)
            return KirbyModes::TechniqueCell(ModesSources(), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()));
        if (f[0] == "epimpill" && f.size() == 3)
            return KirbyModes::AbilityPill(ModesSources(), std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), KirbyModes::AbilityPillSize,
                                           KirbyModes::PauseLook::Magolor);
        if (f[0] == "epimarrow" && f.size() == 3)
            return KirbyModes::AbilityArrow(ModesSources(), f[1] == "1", f[2] == "1", KirbyModes::PauseLook::Magolor);
        if (f[0] == "amiibo")
            return KirbyModes::AmiiboButton(ModesSources(), f.size() == 2);
        if (f[0] == "souv" && f.size() == 4)
            return KirbyModes::SouvenirButton(ModesSources(), std::atoi(f[1].c_str()), f[2] == "1", f[3] == "1",
                                              KirbyModes::SouvenirSize);
        if (f[0] == "rbtn" && f.size() == 4) {
            const auto texts = PlayTexts(f[1]);
            const auto icon = f[2] == "pause" ? KirbyModes::RoundIcon::Pause
                              : f[2] == "continue" ? KirbyModes::RoundIcon::Continue
                                                   : KirbyModes::RoundIcon::Discard;
            return KirbyModes::RoundButton(ModesSources(), icon,
                                           icon == KirbyModes::RoundIcon::Pause      ? texts.pause
                                           : icon == KirbyModes::RoundIcon::Continue ? texts.resume
                                                                                     : texts.discard,
                                           KirbyModes::RoundSize, f[3] == "1");
        }
        if (f[0] == "tab" && f.size() == 3) {
            const int i = std::clamp(std::atoi(f[2].c_str()), 0, 2);
            const auto texts = PlayTexts(f[1]);
            return KirbyModes::TabButton(ModesSources(), i, i == 0 ? texts.game : i == 1 ? texts.abilities : texts.options,
                                         KirbyModes::StageTabSize[0], KirbyModes::StageTabSize[1]);
        }
        if (f[0].starts_with("col"))
            return RenderColour(f);
        if (f[0] == "wother" && f.size() == 2)
            return KirbyModes::OtherPlanetIcon(ModesSources(), f[1] == "1");
        if (f[0] == "wsky")
            return KirbyModes::WorldSky(ModesSources(), 1240, 1080);
        if (f[0] == "credits" && f.size() == 2) {
            const auto i = static_cast<std::size_t>(std::atoi(f[1].c_str()));
            const auto card = i < credit_cards.size() ? KirbyModes::CreditCardImage(ModesSources(), credit_cards[i]) : std::nullopt;
            if (!card)
                return std::nullopt;
            KirbyAssets::Image out{card->width, CreditCanvas, std::vector<std::uint8_t>(std::size_t{card->width} * CreditCanvas * 4)};
            std::copy_n(card->rgba.begin(), std::size_t{card->width} * std::min(card->height, CreditCanvas) * 4, out.rgba.begin());
            return out;
        }
        if (f[0] == "planet" && f.size() == 2)
            return RenderPlanet(f[1] == "1" ? KirbyPlanet::Planet::Halcandra : KirbyPlanet::Planet::PopStar);
        if (f[0] == "magolor")
            return KirbyModes::MagolorPortrait(ModesSources());
        if (f[0] == "max" && f.size() == 2)
            return KirbyModes::Level(ModesSources(), 10, KirbyModes::LevelBox[2], KirbyModes::LevelBox[3],
                                     static_cast<float>(std::atoi(f[1].c_str()) % RainbowSteps) / RainbowSteps);
        if (f[0] == "level" && f.size() == 2)
            return KirbyModes::Level(ModesSources(), std::atoi(f[1].c_str()), KirbyModes::LevelBox[2], KirbyModes::LevelBox[3]);
        const auto src = FilesSources();
        if (f[0] == "panel" && f.size() >= 3) {
            // a complete file's panel shows the page outside its button: drawn for the scale it is
            // shown at (panel/c/<slot>/<scale x 1000>), so those corners line up with the page
            const auto slot = static_cast<std::size_t>(std::clamp(std::atoi(f[2].c_str()), 0, 2));
            const float scale = f.size() > 3 ? std::clamp(std::atoi(f[3].c_str()), 500, 1500) / 1000.0f : 1.0f;
            std::optional<KirbyAssets::Image> backdrop;
            if (f[1] == "c")
                if (const auto page = TitlePage()) {
                    backdrop = KirbyAssets::Image{SlotW, SlotH, std::vector<std::uint8_t>(std::size_t{SlotW} * SlotH * 4)};
                    const float cx = SlotX + SlotW / 2.0f, cy = SlotY[slot] + SlotH / 2.0f;
                    for (std::uint32_t y = 0; y < SlotH; ++y)
                        for (std::uint32_t x = 0; x < SlotW; ++x) {
                            const auto px = static_cast<std::uint32_t>(std::clamp(cx + scale * (SlotX + x + 0.5f - cx), 0.0f, page->width - 1.0f));
                            const auto py = static_cast<std::uint32_t>(std::clamp(cy + scale * (SlotY[slot] + y + 0.5f - cy), 0.0f, page->height - 1.0f));
                            std::copy_n(&page->rgba[(std::size_t{py} * page->width + px) * 4], 4,
                                        &backdrop->rgba[(std::size_t{y} * SlotW + x) * 4]);
                        }
                }
            return KirbyFiles::Panel(src, look(f[1]), SlotW, SlotH, SlotEnd, backdrop ? &*backdrop : nullptr);
        }
        if (f[0] == "rainbow" && f.size() == 2)
            return KirbyFiles::RainbowTile(src, SlotW, SlotH,
                                           static_cast<float>(std::atoi(f[1].c_str()) % RainbowSteps) / RainbowSteps, 4);
        if (f[0] == "dim")
            return KirbyFiles::Shape(src, SlotW, SlotH, SlotEnd, 0x2B000000); // N_Unfocus: about x0.83
        if (f[0] == "flash")
            return KirbyFiles::Shape(src, SlotW, SlotH, SlotEnd, 0xFFFFFFFF); // P_Frash, alpha from the tint
        if (f[0] == "cursor")
            return KirbyFiles::Cursor(src, SlotW, SlotH, SlotEnd, CursorMargin);
        if (f[0] == "kirby" && f.size() == 3)
            return KirbyFiles::Kirby(src, std::atoi(f[1].c_str()), f[2] == "1", KirbySize);
        if (f[0] == "label" && f.size() == 3) {
            const auto msbt = romfs("/msg/Ark/" + f[1] + "/TitleMenu.msbt");
            auto prefix = msbt ? KirbyAssets::MsbtText(*msbt, "FileSelectFileName") : std::nullopt;
            return KirbyFiles::Label(src, prefix ? *prefix : U"File ", Ascii(f[2]), LabelW, LabelH);
        }
        if (f[0] == "pct" && f.size() == 2)
            return KirbyFiles::Percent(src, value(f[1]), PctW, PctH, KirbyFiles::Align::Right);
        if (f[0] == "modes" && f.size() == 3 && f[2].size() == 3)
            return KirbyFiles::Modes(src, look(f[1]), {f[2][0] == '1', false}, {f[2][1] == '1', f[2][2] == '1'}, ModesW, ModesH);
        if (f[0] == "modepct" && f.size() == 4)
            return KirbyFiles::ModePercents(src, look(f[1]), value(f[2]), value(f[3]), ModePctW, ModePctH);
        return std::nullopt;
    }

    // The title page background, made once (complete files' panels cut their surroundings from it).
    std::mutex page_mutex;
    std::shared_ptr<const KirbyAssets::Image> title_page;

    std::shared_ptr<const KirbyAssets::Image> TitlePage() {
        std::scoped_lock lock{page_mutex};
        if (!title_page)
            if (const auto window = assets.Load(romfs, "lyt/Pause/Parts/PauseWinBase00^u"))
                if (auto page = KirbyArt::TitleBackground(*window, 1240, 1080))
                    title_page = std::make_shared<const KirbyAssets::Image>(std::move(*page));
        return title_page;
    }

    // File page pictures are drawn ahead on a background thread as soon as their keys are known,
    // so the page can wait until all of them exist and then appear whole (the host loads module
    // images one at a time).
    std::mutex files_mutex;
    std::condition_variable files_cv;
    std::deque<std::string> files_queue, files_ahead; // needed now / drawn ahead when nothing else waits
    std::unordered_set<std::string> files_queued;
    std::unordered_map<std::string, std::shared_ptr<const KirbyAssets::Image>> files_cache;
    std::unordered_set<std::string> files_served; // fetched by the host
    std::array<std::thread, 2> files_workers;
    bool files_stop{};

    ~Module() {
        {
            std::scoped_lock lock{files_mutex};
            files_stop = true;
        }
        files_cv.notify_all();
        for (auto& worker : files_workers)
            if (worker.joinable())
                worker.join();
    }

    void FilesWorker(int worker) {
        for (;;) {
            std::string key;
            {
                std::unique_lock lock{files_mutex};
                files_cv.wait(lock, [&] { return files_stop || !files_queue.empty() || !files_ahead.empty(); });
                if (files_stop)
                    return;
                auto& queue = files_queue.empty() ? files_ahead : files_queue;
                key = std::move(queue.front());
                queue.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            std::size_t now{}, ahead{};
            {
                std::scoped_lock lock{files_mutex};
                now = files_queue.size(), ahead = files_ahead.size();
            }
            auto image = RenderFiles(key);
            if (logger.log) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                logger.log(logger.userdata, EDEN_DSMOD_LOG_INFO,
                           ("Kirby DSMod drawn " + key + " " + std::to_string(ms) + " ms by worker " + std::to_string(worker) + ", waiting " +
                            std::to_string(now) + " now + " + std::to_string(ahead) + " ahead")
                               .c_str());
            }
            std::scoped_lock lock{files_mutex};
            files_cache[key] = image ? std::make_shared<const KirbyAssets::Image>(std::move(*image)) : nullptr;
        }
    }

    // True once the picture is drawn; otherwise queues it (an urgent one first). Two workers draw them:
    // what a page needs now, then what FilesAhead asked for when nothing else waits, so pictures drawn in
    // advance for later pages never hold up the one shown.
    bool FilesReady(const std::string& key, bool urgent = false) {
        std::scoped_lock lock{files_mutex};
        if (files_cache.contains(key))
            return true;
        if (const auto it = std::find(files_ahead.begin(), files_ahead.end(), key); it != files_ahead.end())
            files_ahead.erase(it), files_queued.erase(key);
        if (files_queued.insert(key).second) {
            if (urgent)
                files_queue.push_front(key);
            else
                files_queue.push_back(key);
            StartWorkers();
            files_cv.notify_one();
        } else if (urgent) {
            // asked for before, now needed on screen: moved ahead of what waits
            if (const auto it = std::find(files_queue.begin(), files_queue.end(), key); it != files_queue.end() && it != files_queue.begin())
                files_queue.erase(it), files_queue.push_front(key);
        }
        return false;
    }

    void FilesAhead(const std::string& key) {
        std::scoped_lock lock{files_mutex};
        if (files_cache.contains(key) || !files_queued.insert(key).second)
            return;
        files_ahead.push_back(key);
        StartWorkers();
        files_cv.notify_one();
    }

    // Drawn already (no queuing): a picture handed to the host before that is drawn on the host's own
    // loading thread, holding up every other picture of the page shown.
    bool FilesDone(const std::string& key) {
        std::scoped_lock lock{files_mutex};
        return files_cache.contains(key);
    }

    void StartWorkers() {
        for (std::size_t i = 0; i < files_workers.size(); ++i)
            if (!files_workers[i].joinable())
                files_workers[i] = std::thread([this, i] { FilesWorker(static_cast<int>(i)); });
    }

    void FilesForget(const std::string& key) {
        std::scoped_lock lock{files_mutex};
        files_cache.erase(key);
        files_queued.erase(key);
        if (const auto it = std::find(files_ahead.begin(), files_ahead.end(), key); it != files_ahead.end())
            files_ahead.erase(it);
    }

    // True once the host has fetched the picture (it shows it a frame or two later). Kept when the module
    // forgets the picture: the host may still hold it, and then never asks again.
    bool FilesServed(const std::string& key) {
        std::scoped_lock lock{files_mutex};
        return files_served.contains(key);
    }

    // A picture that changes while it shows (a counter going up): the new one is handed to the host
    // through a hidden widget first, and takes the old one's place only once the host has it, so the
    // widget never stands empty while it loads.
    struct Swap {
        std::string shown, next;
        u64 served_at{}, asked_at{};
    };
    enum class Draw { Ahead, Normal, Urgent };
    void SwapStep(const EdenDsmodHostApi& host, Swap& swap, const std::string& want, const char* point, const char* preload,
                  u64 tick, Draw draw = Draw::Normal) {
        const auto ready = [&] {
            if (draw != Draw::Ahead)
                return FilesReady(want, draw == Draw::Urgent);
            FilesAhead(want);
            return FilesDone(want);
        };
        if (want == swap.shown)
            swap.next = want; // back before the other one showed
        else if (want != swap.next && ready())
            swap.next = want, swap.served_at = 0, swap.asked_at = tick;
        if (!swap.next.empty() && swap.next != swap.shown) {
            if (swap.shown.empty())
                swap.shown = swap.next; // nothing to keep up yet
            // fetched by the host, or long enough asked for (a picture it had already)
            else if (!swap.served_at && (FilesServed(swap.next) || tick - swap.asked_at > 30))
                swap.served_at = tick;
            else if (swap.served_at && tick - swap.served_at >= 3) {
                FilesForget(swap.shown);
                swap.shown = swap.next;
            }
        }
        if (!swap.next.empty())
            host.publish_text(host.userdata, preload, ("module:kirby:gen/files/" + swap.next).c_str());
        if (!swap.shown.empty())
            host.publish_text(host.userdata, point, ("module:kirby:gen/files/" + swap.shown).c_str());
    }

    std::shared_ptr<const KirbyAssets::Image> FilesImage(std::string_view key) {
        {
            std::scoped_lock lock{files_mutex};
            if (const auto it = files_cache.find(std::string{key}); it != files_cache.end())
                return it->second;
        }
        auto image = RenderFiles(key);
        auto shared = image ? std::make_shared<const KirbyAssets::Image>(std::move(*image)) : nullptr;
        std::scoped_lock lock{files_mutex};
        return files_cache[std::string{key}] = shared;
    }

    // The file select. Per file i, the title menu M keeps a 0x138-byte record: M+0x1FFC+i*0x138
    // its focus flag and M+0x2048+i*0x138 its FileSelectBtn T_Num text pane (text pointer at
    // +0xD8, null for an empty file; UTF-16 digits, a 0x0E tag, '%'). The button's N_CompBtn pane
    // (visible = complete file) sits inline before it in the same layout. The cursor is M+0x2304.
    // Save slots: the save arena R = [main+0x1632500] holds 0x86E0-byte blocks (header magic
    // 0x5544, size at +8, next at +0x18, data at +0x28) from R+0x240, two per slot in slot order.
    std::array<u64, 3> comp_pane{}, nodata_pane{};
    u32 files_cursor{~u32{0}}; // the file last chosen, none yet
    u64 files_frame{}, rainbow_start{}, files_opened{};
    int rainbow_slot{-1};
    bool files_was_on{}, files_was_ready{}, files_prewarmed{};
    std::vector<std::string> files_predicted;
    static constexpr std::size_t PreloadSlots = 48; // keep in sync with FILE_PRELOAD in gen_manifest.py

    // From the intro on, before the buttons' numbers are filled in, draw ahead what the save flags
    // already tell (each slot's panel and mode pictures, both percentage fonts, the labels).
    void PrewarmFiles(const EdenDsmodHostApi& host) {
        if (!files_prewarmed && !title_dir.empty()) {
            files_prewarmed = true;
            const auto want = [&](std::string key) {
                FilesReady(key);
                if (std::find(files_predicted.begin(), files_predicted.end(), key) == files_predicted.end())
                    files_predicted.push_back(std::move(key));
            };
            // the file select first, then what the mode select shows
            want("pct/100");
            want("pct/-");
            want("modepct/n/1/-");
            for (int i = 0; i < 3; ++i) {
                want("label/" + title_dir + "/" + std::to_string(i + 1));
                const auto flags = SlotFlags(host, i);
                if (!flags || (*flags)[0] == 0) {
                    want("panel/d/" + std::to_string(i));
                    want("modes/d/111");
                    want("kirby/1/0");
                    continue;
                }
                const bool magoland = (*flags)[15] != 0;
                // the file's numbers as the file select will show them; its percent is not in the save,
                // (main + Magoland) / 2 is a guess (the page asks for the right one if it differs)
                if (const auto p = SlotProgress(host, i)) {
                    const std::string main = std::to_string(p->main), mag = p->magoland >= 0 ? std::to_string(p->magoland) : "-";
                    want("pct/" + std::to_string(p->magoland >= 0 ? (p->main + p->magoland) / 2 : p->main));
                    for (const char* look : {"n", "c"})
                        if (*look == 'n' || p->main == 100)
                            want(std::string{"modepct/"} + look + "/" + main + "/" + mag);
                    want("kirby/" + std::to_string(p->face) + "/0");
                }
                want("panel/n/" + std::to_string(i));
                want("panel/c/" + std::to_string(i) + "/1000");
                want("panel/c/" + std::to_string(i) + "/920");
                for (const char* look : {"n", "c"})
                    want(std::string{"modes/"} + look + "/0" + (magoland ? "00" : "11"));
                want("kirby/2/0");
                want("kirby/4/1");
            }
            for (int step = 0; step < RainbowSteps; ++step)
                FilesReady("rainbow/" + std::to_string(step));
            want("mainmenu");
            want("magolor");
            // the world map's and the stage page's pictures, drawn ahead when the menus' are done (handed to the
            // host now they would crowd the menus' pictures out of its 64 MB of module images)
            for (const char* key : {"curtain", "wsky", "planet/0", "planet/1", "amiibo", "amiibo/off", "wother/0", "wother/1", "bgmask", "bggear",
                                    "lvicon/Lv1", "lvicon/Lv2", "lvicon/Lv3", "lvicon/Lv4", "lvicon/Lv5", "lvicon/Lv6", "lvicon/Lv7", "lvicon/Ship",
                                    "ablarrow/1/0", "ablarrow/0/0", "ablarrow/1/1", "ablarrow/0/1"})
                FilesAhead(key);
            for (const char* id : {"pause/0", "continue/0", "discard/0", "discard/1"})
                FilesAhead("rbtn/" + title_dir + "/" + id);
            for (const char* part : {"charging", "full", "particles1", "particles2", "glow", "edge", "front", "locked"})
                FilesAhead(std::string{"epigauge/"} + part);
            for (int k = 0; k < KirbyModes::GaugeSegments; ++k)
                FilesAhead("epigauge/empty/" + std::to_string(k));
            for (int t = 0; t < 3; ++t)
                FilesAhead("tab/" + title_dir + "/" + std::to_string(t));
            for (const char* mode : {"0", "1"}) {
                for (int item = 0; item <= 5; ++item)
                    FilesAhead("souv/" + std::to_string(item) + "/" + mode + "/0");
                FilesAhead(std::string{"souv/0/"} + mode + "/1");
            }
            for (const auto& slot : KirbyModes::MapSlots)
                FilesAhead(WorldCursorKey(slot));
            // Merry Magoland's page: its doors are the slowest pictures (each a 3D render)
            for (const char* key : {"mago/bg", "mago/arrow/0", "mago/arrow/1"})
                FilesAhead(key);
            for (int i = 0; i < KirbyMago::Sections; ++i)
                FilesAhead("mago/title/" + title_dir + "/" + std::to_string(i)), FilesAhead("mago/dots/" + std::to_string(i));
            for (int i = 0; i < KirbyGates::Doors; ++i)
                FilesAhead("mago/door/" + std::to_string(i));
            FilesAhead("mago/globe");
        }
        // the dark and title pages load these through invisible widgets (files.pre.<n>), each once it is drawn
        for (std::size_t n = 0; n < files_predicted.size() && n < PreloadSlots; ++n)
            if (FilesDone(files_predicted[n]))
                host.publish_text(host.userdata, ("files.pre." + std::to_string(n)).c_str(),
                                  ("module:kirby:gen/files/" + files_predicted[n]).c_str());
    }

    std::optional<std::u32string> SlotPercent(const EdenDsmodHostApi& host, u64 pane) const {
        char name[8]{};
        u64 text{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0xB0, name, 7) ||
            std::string_view{name} != "T_Num" ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0xD8, &text, 8))
            return std::nullopt;
        std::u32string digits;
        std::uint16_t c[8]{};
        if (text != 0 && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, text, c, sizeof c))
            for (const auto ch : c) {
                if (ch < '0' || ch > '9')
                    break;
                digits += static_cast<char32_t>(ch);
            }
        return digits;
    }

    // A FileSelectBtn pane found by name near the slot's T_Num (the panes sit inline in the
    // button's layout, N_CompBtn before T_Num and N_NoData after it), cached per slot; true when
    // the game shows it.
    bool SlotPaneShown(const EdenDsmodHostApi& host, std::array<u64, 3>& cache, int i, u64 num_pane,
                       std::string_view name, bool before) {
        static constexpr u64 Span = 0x4000, NameAt = 0xB0;
        const auto named = [&](u64 pane) {
            char text[16]{};
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + NameAt, text, 15) &&
                   std::string_view{text} == name;
        };
        if (cache[i] == 0 || !named(cache[i])) {
            cache[i] = 0;
            const u64 from = before ? num_pane - Span : num_pane;
            std::vector<char> bytes(Span);
            if (num_pane > Span && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, from, bytes.data(), Span)) {
                const std::string_view all{bytes.data(), bytes.size()}, wanted{name.data(), name.size() + 1};
                const auto at = before ? all.rfind(wanted) : all.find(wanted);
                if (at != std::string_view::npos && at >= NameAt)
                    cache[i] = from + at - NameAt;
            }
        }
        u8 flags{};
        return cache[i] != 0 &&
               dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, cache[i] + 0x58, &flags, 1) &&
               (flags & 1) != 0;
    }

    // A slot's save data (the save file's bytes) in the arena's block list, or 0.
    // `live`: the second block of the file's pair, the one the game updates while it runs
    u64 SlotData(const EdenDsmodHostApi& host, int i, bool live = false) const {
        u64 arena{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x1632500, &arena, 8))
            return 0;
        u64 block = arena + 0x240;
        const int last = 2 * i + (live ? 1 : 0);
        for (int k = 0; k <= last; ++k) {
            u64 head[4]{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, block, head, sizeof head) ||
                (head[0] & 0xFFFF) != 0x5544 || head[1] != 0x86E0)
                return 0;
            if (k < last)
                block = head[3];
        }
        return block + 0x28;
    }

    // Per-mode percentages for the file page (the package's own rule): main mode = story 50 + extra
    // 30 + Arena 5 + Magolor Epilogue 10 + True Arena 5, Magoland = missions 90 + masks 10, each
    // part rounded up. -1: Magoland is still locked. Story and extra completion are worked out as
    // the game's mode select does (main+0x52CAEC): 0.35 per energy sphere (the stage table at
    // main+0x132E7B4 says how many each stage has), 6 per world flag, 7 for the clear and 1 per
    // challenge stage, up to 100.
    struct Progress {
        int main{}, magoland{-1};
        int face{2}; // FileSelectKbIcon: 2 started, 3 story cleared, 4 everything but Magoland
        // the mode select page
        KirbyModes::Mode story, extra;
        KirbyModes::Epilogue epilogue;
        KirbyModes::Magoland attractions;
        // the main mode's menu: per mode, the worlds open and each world's spheres (got, of)
        struct Worlds {
            int open{};
            std::array<int, KirbyModes::Worlds> got{}, of{};
            std::array<bool, KirbyModes::Worlds> beaten{};
            std::array<int, KirbyModes::Challenges> medals{};
        };
        std::array<Worlds, 2> worlds;
        KirbyModes::EpiloguePage epi; // without its texts
        std::array<KirbyModes::ArenaPage, 2> arena; // the Arena, the True Arena (without their texts)
        std::array<bool, 2> arena_open{};
    };

    // MahoroaMode/BadgeRank.bin: per Epilogue stage (the game's stage table, main+0x16047F0) the
    // scores for platinum, gold and silver.
    mutable std::optional<std::vector<std::array<u32, 3>>> badge_ranks;
    const std::vector<std::array<u32, 3>>& BadgeRanks() const {
        if (!badge_ranks) {
            badge_ranks.emplace();
            if (const auto bin = romfs("/yaml/Scn/Step/MahoroaMode/BadgeRank.bin")) {
                static constexpr u32 Head[]{6, 4, 0x10, 0x14, 0x18, 0x1C};
                for (std::size_t at = 0; at + 56 <= bin->size(); at += 4) {
                    if (std::memcmp(bin->data() + at, Head, sizeof Head) != 0)
                        continue;
                    u32 v[8]{};
                    std::memcpy(v, bin->data() + at + 24, sizeof v);
                    badge_ranks->push_back({v[1], v[3], v[5]});
                }
            }
        }
        return *badge_ranks;
    }

    // The Epilogue's stages, by their id in the game's table: area n's stages, boss, then its three
    // trials (Area1Stage1 = 1 .. Area4Pow3 = 31), the extra stage 32, the last stages 34 and 35.
    // Save: completed flags bytes at 0xFE3 + id, high scores u32 at 0x10A4 + 4 id (both validated in
    // game); techniques' learnt flags at 0xF9C and levels at 0xFA8 (Vitality's at index 11,
    // Craftiness's at 10).
    template <typename U32At>
    KirbyModes::EpiloguePage EpilogueOf(const std::vector<u8>& d, U32At u32_at) const {
        static constexpr int First[KirbyModes::Areas]{1, 8, 16, 24}, Count[KirbyModes::Areas]{3, 4, 4, 4};
        static constexpr int Max[KirbyModes::Techniques]{9, 6, 6, 6, 6, 3, 3, 3, 3, 6, 9, 9};
        // the trials' techniques (msbt order) and the stage each door opens after (EnhancingStage.bin)
        static constexpr int Trial[KirbyModes::Areas][3]{{0, 9, 10}, {3, 2, 1}, {4, 5, 8}, {6, 7, 11}};
        static constexpr int Opens[KirbyModes::Areas][3]{{2, 3, 3}, {2, 3, 4}, {2, 3, 4}, {2, 3, 4}};
        const auto save_index = [](int t) { return t == 10 ? 11 : t == 11 ? 10 : t; };
        const auto done = [&](int id) { return d[0xFE3 + id] != 0; };
        const auto& ranks = BadgeRanks();
        const auto medal = [&](int id) {
            if (!done(id))
                return -1;
            const u32 score = u32_at(0x10A4 + id * 4);
            if (static_cast<std::size_t>(id) >= ranks.size())
                return 0;
            const auto& r = ranks[id];
            return score >= r[0] ? 3 : score >= r[1] ? 2 : score >= r[2] ? 1 : 0;
        };
        KirbyModes::EpiloguePage e;
        e.magic = static_cast<int>(u32_at(0xF94));
        e.sprout = d[0x2379] ? 2 : done(34) || done(35) ? 1 : 0;
        for (int t = 0; t < KirbyModes::Techniques; ++t) {
            e.max[t] = Max[t];
            e.level[t] = d[0xF9C + save_index(t)] ? static_cast<int>(u32_at(0xFA8 + save_index(t) * 4)) : 0;
        }
        // the extra stage opens with gold or better everywhere and every trial done
        bool all_gold = true;
        for (int a = 0; a < KirbyModes::Areas; ++a)
            for (int k = 0; k <= Count[a]; ++k)
                all_gold = all_gold && medal(First[a] + k) >= 2;
        bool all_trials = true;
        for (int a = 0; a < KirbyModes::Areas; ++a)
            for (int k = 0; k < 3; ++k)
                all_trials = all_trials && done(First[a] + Count[a] + 1 + k);
        for (int a = 0; a < KirbyModes::Areas; ++a) {
            // an area opens once the one before it is beaten, a stage once the one before it is done
            if (a > 0 && !done(First[a - 1] + Count[a - 1]))
                break;
            ++e.open;
            auto& area = e.areas[a];
            bool before = true;
            for (int k = 0; k < Count[a]; ++k) {
                area.medals.push_back(before ? medal(First[a] + k) : -2);
                before = done(First[a] + k);
            }
            area.boss = medal(First[a] + Count[a]);
            if (a == KirbyModes::Areas - 1)
                area.medals.push_back(done(32) || (all_gold && all_trials) ? medal(32) : -2);
            for (int k = 0; k < 3; ++k) {
                area.trials[k] = Trial[a][k];
                const int id = First[a] + Count[a] + 1 + k;
                area.trial_state[k] = done(id) ? 2 : done(First[a] + Opens[a][k] - 1) ? 1 : 0;
            }
        }
        return e;
    }

    std::optional<Progress> SlotProgress(const EdenDsmodHostApi& host, int i) const {
        static constexpr std::size_t Size = 0x86D8;
        const u64 data = SlotData(host, i);
        std::vector<u8> d(Size);
        u32 stages[0x27 * 3]{};
        if (data == 0 || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data, d.data(), Size) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x132E7B4, stages, sizeof stages))
            return std::nullopt;
        const auto u32_at = [&](std::size_t o) {
            u32 v{};
            std::memcpy(&v, &d[o], 4);
            return v;
        };
        // in hundredths: 120 spheres at 0.35 summed as floats fall just short of 42
        const auto completion = [&](std::size_t mode) {
            int total = 0;
            for (std::size_t stage = 0; stage < 0x27; ++stage)
                for (u32 sphere = 0; sphere < std::min<u32>(stages[stage * 3 + 1], 6); ++sphere)
                    total += d[mode + stage * 6 + 9 + sphere] ? 35 : 0;
            for (std::size_t o = 0xF8; o < 0x130; o += 8)
                total += d[mode + o] ? 600 : 0;
            total += d[mode + 1] ? 700 : 0;
            for (std::size_t k = 0; k < 12; ++k)
                total += u32_at(mode + 0x17C + k * 0x14) > 0 ? 100 : 0;
            if (total > 0 && total < 100)
                total = 100;
            return std::min(100, total / 100);
        };
        const auto share = [](int weight, int done, int of) { return (weight * done + of - 1) / of; };
        Progress out;
        const int story = completion(0), extra = completion(0x6C8);
        out.main = share(50, story, 100) + share(30, extra, 100) + (u32_at(0xD94) == 3 ? 5 : 0) +
                   (d[0x2379] ? 10 : d[0x2378] ? 5 : 0) + (u32_at(0xE5C) == 3 ? 5 : 0);
        // FileSelectBtn's Kirby (main+0xFE68F8 over the state main+0x1046E80 fills): a cleared story
        // (header byte 1) shows face 3, or face 4 once the game's own main-mode score is 100
        if (d[1]) {
            const float game_main = 0.4f * story + 0.4f * extra + (d[0x2379] ? 10.0f : d[0x2378] ? 5.0f : 0.0f) +
                                    (u32_at(0xD94) == 3 ? 5.0f : 0.0f) + (u32_at(0xE5C) == 3 ? 5.0f : 0.0f);
            out.face = game_main >= 100 ? 4 : 3;
        }
        // per mode: lives u32 +0x26C, point stars +0x270; the Lor's parts come back one per world
        // boss of the first five (the world flags), in PauseShipPic_Wait<n>'s order
        const auto mode = [&](std::size_t base, bool locked) {
            KirbyModes::Mode m{locked, completion(base), static_cast<int>(u32_at(base + 0x26C)),
                               static_cast<int>(u32_at(base + 0x270)), 0, 0};
            for (std::size_t stage = 0; stage < 0x27; ++stage)
                for (u32 sphere = 0; sphere < std::min<u32>(stages[stage * 3 + 1], 6); ++sphere)
                    m.spheres += d[base + stage * 6 + 9 + sphere] != 0;
            for (std::size_t k = 0; k < 5; ++k)
                m.parts += d[base + 0xF8 + k * 8] != 0;
            return m;
        };
        out.story = mode(0, false);
        out.extra = mode(0x6C8, d[1] == 0);
        // the stage table's first word is the world; a world opens once the one before it is
        // beaten (its world flag)
        for (int m = 0; m < 2; ++m) {
            const std::size_t base = m ? 0x6C8 : 0;
            auto& w = out.worlds[m];
            for (std::size_t stage = 0; stage < 0x27; ++stage) {
                const u32 world = stages[stage * 3], spheres = std::min<u32>(stages[stage * 3 + 1], 6);
                if (world >= KirbyModes::Worlds)
                    continue;
                w.of[world] += static_cast<int>(spheres);
                for (u32 sphere = 0; sphere < spheres; ++sphere)
                    w.got[world] += d[base + stage * 6 + 9 + sphere] != 0;
            }
            w.open = 1;
            while (w.open < KirbyModes::Worlds && d[base + 0xF8 + (w.open - 1) * 8])
                ++w.open;
            for (int k = 0; k < KirbyModes::Worlds; ++k)
                w.beaten[k] = d[base + 0xF8 + k * 8] != 0;
            // the challenges' records (+0x17C, 0x14 each) follow the challenge stages Stage01..12 (02, 05
            // and 09 do not exist): state u32 (0 not played), medal u32 (1 copper .. 4 platinum, unverified)
            static constexpr std::size_t Record[KirbyModes::Challenges]{0, 2, 3, 5, 6, 9, 10, 11, 7};
            for (int k = 0; k < KirbyModes::Challenges; ++k) {
                const std::size_t o = base + 0x17C + Record[k] * 0x14;
                const u32 medal = u32_at(o + 4);
                w.medals[k] = u32_at(o) == 0 ? -1 : medal <= 4 ? static_cast<int>(medal) : 0;
            }
        }
        // Magolor Epilogue: magic points u32 0xF94 (file 1 shows 16229), cleared 0x2379
        // open once 0x2378 is set (the game shows the Epilogue then, whatever the story's state)
        out.epilogue = {d[0x2378] == 0, d[0x2379] != 0, static_cast<int>(u32_at(0xF94)), 0};
        // Magolor's level (the package's own rule): from the sum of his 12 techniques' levels (u32 at
        // 0xFA8), 7 at the Epilogue's start and 69 with everything bought
        static constexpr int LevelFrom[]{7, 15, 22, 26, 32, 35, 39, 45, 57, 69};
        int techniques = 0;
        for (std::size_t k = 0; k < 12; ++k)
            techniques += static_cast<int>(u32_at(0xFA8 + k * 4));
        for (const int from : LevelFrom)
            out.epilogue.level += techniques >= from;
        out.epi = EpilogueOf(d, u32_at);
        // Arena at 0xD90, True Arena at 0xE58: u32 state (3 beaten), u32 state, then six records of 0x20
        // (three alone, three together): u32 used, rounds reached, time in frames, four characters
        // (11 = none) - validated against the game's record screen (10:47,55 = 38853 frames)
        for (int a = 0; a < 2; ++a) {
            const std::size_t base = a ? 0xE58 : 0xD90;
            auto& page = out.arena[a];
            page.final = a == 1;
            page.rounds = a ? 21 : 13;
            page.cleared = u32_at(base + 4) == 3;
            out.arena_open[a] = u32_at(base) != 0 || u32_at(base + 4) != 0;
            for (int r = 0; r < 6; ++r) {
                const std::size_t o = base + 8 + r * 0x20;
                auto& rec = page.records[r];
                if (u32_at(o) == 0)
                    continue;
                rec.rounds = static_cast<int>(u32_at(o + 4));
                rec.frames = static_cast<int>(u32_at(o + 8));
                for (int p = 0; p < 4; ++p) {
                    const u32 who = u32_at(o + 12 + p * 4);
                    rec.players[p] = who < 7 ? static_cast<int>(who) : -1;
                }
                // byte +29: a souvenir used (validated); +28: an amiibo used (unverified)
                rec.amiibo = d[o + 28] != 0;
                rec.gift = d[o + 29] != 0;
            }
        }
        // the missions done per set, the game's list's (each game's own, the extra ones, the final extra ones)
        out.attractions.locked = d[15] == 0;
        u32 phases[120]{};
        for (std::size_t k = 0; k < 120; ++k)
            phases[k] = u32_at(0x2B04 + k * 4);
        const auto per_set = MagoDone(phases);
        std::copy(per_set.begin(), per_set.end(), out.attractions.done.begin());
        // the final extra missions open once the first hundred are done
        int first = 0;
        for (int g = 0; g + 1 < KirbyModes::Attractions; ++g)
            first += out.attractions.done[g];
        out.attractions.finals = first == 100 || out.attractions.done[KirbyModes::Attractions - 1] > 0;
        if (d[15]) {
            int missions = 0, masks = 0;
            for (std::size_t k = 0; k < 120; ++k)
                missions += u32_at(0x2B04 + k * 4) > 0;
            for (std::size_t k = 0; k < 86; ++k)
                masks += u32_at(0x2D14 + k * 4) > 0;
            out.magoland = share(90, std::min(missions, 100), 100) + share(10, masks, 86);
        }
        return out;
    }

    std::array<std::optional<Progress>, 3> progress;
    // The main mode's menu: the section under its cursor shown in two sets of pictures (its window and
    // its name). On a move, the set showing the old section slides its window out on the side away from
    // the new button and the other set slides the new one in from the opposite side, the names
    // crossfading. Each set has one window group per side (the runtime slides a group from a fixed
    // side); the module shows the one it needs.
    struct Slide {
        struct Pictures {
            std::string panel, title;
            std::string cursor; // the menu's small buttons: their cursor outline, which pulses
            bool bounce{};      // ...and whether they bounce when focused (a locked one does not)
        };
        Pictures shown;
        int side{}; // 0 left, 1 right, 2 top, 3 bottom: where it comes from / goes to
        bool open{};
        u64 focused_at{}, pushed_at{~u64{0}};
    };
    // the shown file's story, extra, Epilogue, Arena and True Arena pictures, once drawn
    std::array<Slide::Pictures, 5> adventure;
    std::string magolor_level;                // Magolor's level picture on the Epilogue page
    bool jukebox_open{};                      // opened with the story's clear
    std::atomic<bool> menu_tap{false};
    std::array<Slide, 2> slides;
    int slide_set{};
    u32 slide_button{~u32{0}};

    // Cursor ids (M+0x3EF4) and the buttons' centres on the top screen (1280 x 720): 0 story, 1 Arena,
    // 2 Magolor Epilogue, 3 theater, 4 jukebox, 5 Merry Magoland, 6 previous adventures, 7 extra,
    // 8 True Arena.
    static constexpr std::array<std::array<int, 2>, 9> ButtonAt{{{400, 235}, {268, 447}, {640, 455}, {485, 610}, {785, 610},
                                                                {1105, 610}, {170, 610}, {880, 235}, {1010, 447}}};

    void SlideStep(const EdenDsmodHostApi& host, bool shown, u32 button) {
        const auto section = [&](u32 b) {
            if (b >= 3 && b <= 6 && !title_dir.empty()) {
                // the small buttons: the menu's title, the button and its cursor; the jukebox opens with the story
                const std::string id = std::to_string(b);
                return Slide::Pictures{"mbtn/" + title_dir + "/" + id, "advt/" + title_dir + "/5", "mcur/" + id,
                                       b != 4 || jukebox_open};
            }
            return b == 0 ? adventure[0] : b == 7 ? adventure[1] : b == 2 ? adventure[2] : b == 1 ? adventure[3]
                 : b == 8 ? adventure[4] : Slide::Pictures{};
        };
        if (!shown) {
            slide_button = ~u32{0};
        } else if (slide_button == ~u32{0} || button >= ButtonAt.size() || slide_button >= ButtonAt.size()) {
            // the menu just opened (or an unknown button): the section is there at once
            slides[slide_set] = {section(button), 1, !section(button).panel.empty()};
            slides[slide_set].focused_at = files_frame;
            slides[slide_set ^ 1].open = false;
            slide_button = button;
        } else if (button != slide_button) {
            const int dx = ButtonAt[button][0] - ButtonAt[slide_button][0], dy = ButtonAt[button][1] - ButtonAt[slide_button][1];
            // the old section leaves on the side away from the new button, the new one comes from the other:
            // up or down when the cursor changes row, sideways along a row
            const int leave = std::abs(dy) > 100 ? (dy > 0 ? 2 : 3) : (dx > 0 ? 0 : 1);
            slides[slide_set].side = leave;
            slides[slide_set].open = false;
            slide_set ^= 1;
            slides[slide_set] = {section(button), leave ^ 1, !section(button).panel.empty()};
            slides[slide_set].focused_at = files_frame;
            slide_button = button;
        } else if (slides[slide_set].shown.panel != section(button).panel) {
            // the same button, its pictures drawn later or the file's progress changed
            slides[slide_set].shown = section(button);
            slides[slide_set].open = !slides[slide_set].shown.panel.empty();
        }
        // a tap on a small button: the game's A, and the button's Push
        const bool tappable = shown && !slides[slide_set].shown.cursor.empty();
        if (menu_tap.exchange(false) && tappable) {
            Press("a");
            slides[slide_set].pushed_at = files_frame;
        }
        host.publish_i64(host.userdata, "adv.tap_on", tappable ? 1 : 0);
        if (!host.publish_text)
            return;
        for (int s = 0; s < 2; ++s) {
            const std::string p = std::string{"adv."} + "ab"[s] + ".";
            const auto& slide = slides[s];
            // ModeSelectBtn*_Focus (1 to 1.12 to 1.08 in 6 frames) and _Push (down to 0.92 and back in 8);
            // the cursor fades in over 6 frames, then ModeSelectBtnCursor_Wait (255 to 0 and back in 60)
            float scale = 1, alpha = 1;
            if (!slide.shown.cursor.empty()) {
                const float f = static_cast<float>(files_frame - slide.focused_at);
                if (slide.shown.bounce)
                    scale = f < 2 ? 1 + 0.06f * f : f < 6 ? 1.12f - 0.01f * (f - 2) : 1.08f;
                if (files_frame >= slide.pushed_at) {
                    const float q = static_cast<float>(files_frame - slide.pushed_at);
                    if (q < 8)
                        scale = q < 3 ? 1.08f - 0.16f * q / 3 : 0.92f + 0.16f * (q - 3) / 5;
                }
                alpha = f < 6 ? f / 6 : 0.5f + 0.5f * std::cos((f - 6) * 6.2831853f / 60);
            }
            host.publish_i64(host.userdata, (p + "scale").c_str(), static_cast<std::int64_t>(std::lround(scale * 1000)));
            host.publish_i64(host.userdata, (p + "pulse").c_str(), std::lround(alpha * 15));
            if (!slide.shown.cursor.empty())
                host.publish_text(host.userdata, (p + "cursor").c_str(), ("module:kirby:gen/files/" + slide.shown.cursor).c_str());
            if (!slide.shown.panel.empty()) {
                host.publish_text(host.userdata, (p + "panel").c_str(), ("module:kirby:gen/files/" + slide.shown.panel).c_str());
                host.publish_text(host.userdata, (p + "title").c_str(), ("module:kirby:gen/files/" + slide.shown.title).c_str());
            }
            host.publish_i64(host.userdata, (p + "open").c_str(), slide.open ? 1 : 0);
            host.publish_i64(host.userdata, (p + "shown").c_str(), slide.shown.panel.empty() ? 0 : 1);
            static constexpr const char* Sides[]{"left", "right", "top", "bottom"};
            // the Epilogue's page has Magolor's picture and his level over it, in their own widgets
            const bool epilogue = slide.shown.panel.starts_with("epi/");
            for (int side = 0; side < 4; ++side) {
                const bool on = slide.side == side && !slide.shown.panel.empty();
                host.publish_i64(host.userdata, (p + Sides[side]).c_str(), on ? 1 : 0);
                host.publish_i64(host.userdata, (p + Sides[side] + "_m").c_str(), on && epilogue ? 1 : 0);
                host.publish_i64(host.userdata, (p + Sides[side] + "_b").c_str(), on && !slide.shown.cursor.empty() ? 1 : 0);
            }
            if (epilogue && !magolor_level.empty())
                host.publish_text(host.userdata, (p + "level").c_str(), ("module:kirby:gen/files/" + magolor_level).c_str());
        }
    }

    std::string ModesKey(const Progress& p) const {
        const auto mode = [](const KirbyModes::Mode& m) {
            return std::to_string(m.locked) + "." + std::to_string(m.percent) + "." + std::to_string(m.lives) + "." +
                   std::to_string(m.stars) + "." + std::to_string(m.spheres) + "." + std::to_string(m.parts);
        };
        std::string mago = std::to_string(p.attractions.locked) + "." + std::to_string(p.attractions.finals);
        for (const int n : p.attractions.done)
            mago += "." + std::to_string(n);
        return "mode/" + title_dir + "/" + mode(p.story) + "/" + mode(p.extra) + "/" +
               std::to_string(p.epilogue.locked) + "." + std::to_string(p.epilogue.complete) + "." +
               std::to_string(p.epilogue.magic) + "/" + mago;
    }

    // <msg dir>/<0 story, 1 extra>/<lives>.<stars>.<parts>.<open>/<got per world>/<of per world>/<beaten per
    // world>/<medal per challenge>
    std::string AdventureKey(const Progress& p, int m) const {
        const auto& mode = m ? p.extra : p.story;
        const auto& w = p.worlds[m];
        std::string got, of, beaten, medals;
        for (int k = 0; k < KirbyModes::Worlds; ++k) {
            got += (k ? "." : "") + std::to_string(w.got[k]);
            of += (k ? "." : "") + std::to_string(w.of[k]);
            beaten += (k ? "." : "") + std::to_string(w.beaten[k]);
        }
        for (int k = 0; k < KirbyModes::Challenges; ++k)
            medals += (k ? "." : "") + std::to_string(w.medals[k]);
        return title_dir + "/" + std::to_string(m) + "/" + std::to_string(mode.lives) + "." +
               std::to_string(mode.stars) + "." + std::to_string(mode.parts) + "." + std::to_string(w.open) + "/" + got +
               "/" + of + "/" + beaten + "/" + medals;
    }

    // epi/<msg dir>/<magic>.<sprout>.<areas open>/<technique levels>/<area>/<area>/<area>/<area>, each
    // area <boss>.<trial states x3>.<stage medals...>
    std::string EpilogueKey(const Progress& p) const {
        const auto& e = p.epi;
        std::string key = "epi/" + title_dir + "/" + std::to_string(e.magic) + "." + std::to_string(e.sprout) + "." +
                          std::to_string(e.open) + "/";
        for (int t = 0; t < KirbyModes::Techniques; ++t)
            key += (t ? "." : "") + std::to_string(e.level[t]);
        for (const auto& a : e.areas) {
            key += "/" + std::to_string(a.boss);
            for (const int st : a.trial_state)
                key += "." + std::to_string(st);
            for (const int m : a.medals)
                key += "." + std::to_string(m);
        }
        return key;
    }

    // arena/<msg dir>/<final>/<cleared>/<record> x 6, each <frames>.<rounds>.<4 characters>.<gift>.<amiibo>
    std::string ArenaKey(const Progress& p, int a) const {
        const auto& page = p.arena[a];
        std::string key = "arena/" + title_dir + "/" + std::to_string(a) + "/" + std::to_string(page.cleared);
        for (const auto& r : page.records) {
            key += "/" + std::to_string(r.frames) + "." + std::to_string(r.rounds);
            for (const int who : r.players)
                key += "." + std::to_string(who);
            key += "." + std::to_string(r.gift) + "." + std::to_string(r.amiibo);
        }
        return key;
    }

    // The menu keeps the name of its music at M+0x128: the mode select always plays
    // BGM_LP_LEVELOPEN1, the main mode's own menu a track that depends on the file's progress
    // (BGM_LP_NORMAL1, BGM_LP_MENU1, ...). Its layouts move between menu rebuilds, the name does not.
    bool MainModeMenu(const EdenDsmodHostApi& host, u64 menu) const {
        const std::string bgm = MenuMusic(host, menu);
        return bgm.starts_with("BGM_") && bgm != "BGM_LP_LEVELOPEN1";
    }

    std::string MenuMusic(const EdenDsmodHostApi& host, u64 menu) const {
        char name[32]{};
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, menu + 0x128, name, sizeof name - 1) ? name : "";
    }

    // The mode select shows the file the cursor stayed on; each file's page is drawn from the file
    // select on, so it is ready when the game gets there.
    bool ModesStep(const EdenDsmodHostApi& host, u32 cursor, int screen) {
        bool ready = false;
        for (int i = 0; i < 3; ++i) {
            // a file only just created keeps its started flag clear until the game first saves it: the
            // one the player chose is shown all the same
            const auto flags = SlotFlags(host, i);
            if (!flags || ((*flags)[0] == 0 && (static_cast<u32>(i) != cursor || screen != 4)) || title_dir.empty())
                continue;
            if (files_frame % 30 == 1 || !progress[i])
                progress[i] = SlotProgress(host, i);
            if (!progress[i])
                continue;
            // on the file select these wait behind its own pictures, the file under the cursor first
            const auto want = [&](const std::string& key, bool urgent = false) {
                if (screen == 4)
                    return FilesReady(key, urgent);
                if (static_cast<u32>(i) == cursor)
                    return FilesReady(key);
                FilesAhead(key);
                return FilesDone(key);
            };
            const auto key = ModesKey(*progress[i]);
            // MAX turns the complete files' rainbow through its letters, at their pace
            const bool max = progress[i]->epilogue.level >= 10;
            const auto level = max ? "max/" + std::to_string(files_frame * RainbowSteps / RainbowFrames % RainbowSteps)
                                   : "level/" + std::to_string(progress[i]->epilogue.level);
            if (max) {
                max_wanted = true;
                for (int step = 0; step < RainbowSteps; ++step)
                    want("max/" + std::to_string(step));
            }
            const bool drawn = want(key, true) && want(max ? std::string{"max/0"} : level, true);
            if (static_cast<u32>(i) != cursor)
                continue;
            ready = drawn && screen == 4 && modes_ok;
            // handed over once drawn (one asked for before is drawn on the host's own loading thread)
            if (FilesDone(key))
                host.publish_text(host.userdata, "modes.page", ("module:kirby:gen/files/" + key).c_str());
            if (FilesDone(level))
                host.publish_text(host.userdata, "modes.level", ("module:kirby:gen/files/" + level).c_str());
            host.publish_i64(host.userdata, "modes.magolor", progress[i]->epilogue.locked ? 0 : 1);
            magolor_level = level;
            jukebox_open = !progress[i]->extra.locked;
            if (screen == 4 && !progress[i]->epilogue.locked)
                EpiPrewarm(host, i, menu_button == 2);
            // the chosen file's world map as a game first shows it, on either planet Kirby may be on
            if (screen == 4)
                for (int mode = 0; mode < 2; ++mode) {
                    const auto& m = mode ? progress[i]->extra : progress[i]->story;
                    if (m.locked)
                        continue;
                    for (int planet = 0; planet <= (progress[i]->worlds[mode].open >= 6 ? 1 : 0); ++planet)
                        FilesAhead(WorldMapKey(planet, mode, std::min(m.lives, 999), std::min(m.stars, 9999), m.spheres));
                }
            for (const char* id : {"3", "4", "5", "6"}) {
                FilesReady("mbtn/" + title_dir + "/" + id);
                FilesReady(std::string{"mcur/"} + id);
            }
            FilesReady("advt/" + title_dir + "/5");
            for (int m = 0; m < 5; ++m) {
                const auto panel = m >= 3 ? ArenaKey(*progress[i], m - 3)
                                   : m == 2 ? EpilogueKey(*progress[i])
                                            : "advp/" + AdventureKey(*progress[i], m);
                const auto title = "advt/" + title_dir + "/" + std::to_string(m);
                const bool open = m == 0 || (m == 1   ? !progress[i]->extra.locked
                                             : m == 2 ? !progress[i]->epilogue.locked
                                                      : progress[i]->arena_open[m - 3]);
                // both drawn ahead, whichever the cursor is on
                const bool drawn = open && want(panel, true) && want(title, true);
                adventure[m] = drawn ? Slide::Pictures{panel, title} : Slide::Pictures{};
            }
        }
        return ready;
    }

    std::optional<std::array<u8, 16>> SlotFlags(const EdenDsmodHostApi& host, int i) const {
        u64 arena{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x1632500, &arena, 8))
            return std::nullopt;
        u64 block = arena + 0x240;
        for (int k = 0; k <= 2 * i; ++k) {
            u64 head[4]{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, block, head, sizeof head) ||
                (head[0] & 0xFFFF) != 0x5544 || head[1] != 0x86E0)
                return std::nullopt;
            if (k < 2 * i)
                block = head[3];
        }
        std::array<u8, 16> flags{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, block + 0x28, flags.data(), flags.size()))
            return std::nullopt;
        return flags;
    }

    // The buttons' own animations, replayed from the cursor: Focus / Unfocus on every move, the
    // cursor outline fading in and blinking, and Push (with the flash and the outline burst) only
    // when a file is chosen by touch. A tap first walks the game's cursor there, then presses A.
    std::array<u64, 3> moved_at{};
    std::array<u64, 3> pushed_at{~u64{0} >> 1, ~u64{0} >> 1, ~u64{0} >> 1};
    int shown_cursor{-1}, walk_to{-1};
    bool files_boost{}, title_boost{};
    u64 walk_until{}, walk_wait{};

    void FilesMotion(const EdenDsmodHostApi& host, u64 menu, u32 cursor, bool shown) {
        const u64 now = files_frame;
        if (!shown || cursor > 2) {
            shown_cursor = -1, walk_to = -1;
            return;
        }
        if (shown_cursor < 0) {
            // the page appears settled: no focus animation for the file the game opened on
            shown_cursor = static_cast<int>(cursor);
            moved_at.fill(now - 1000);
        } else if (shown_cursor != static_cast<int>(cursor)) {
            moved_at[shown_cursor] = moved_at[cursor] = now;
            shown_cursor = static_cast<int>(cursor);
            files_boost = !files_boost;
        }
        if (const int tap = file_tap.exchange(-1); tap >= 0 && tap <= 2 && walk_to < 0) {
            walk_to = tap, walk_until = now + 180, walk_wait = 0;
            // the tapped file opens at once: the menu's cursor is set to it, then A is pressed;
            // if the game does not take the new cursor, the cursor is walked there instead
            // (the cursor index for A, and each file's focus flag, which the pad moves from)
            const u32 target = static_cast<u32>(tap), on = 1, off = 0;
            if (static_cast<int>(cursor) != tap && host.write_memory &&
                host.write_memory(host.userdata, menu + 0x1FFC + cursor * 0x138, &off, 4) &&
                host.write_memory(host.userdata, menu + 0x1FFC + target * 0x138, &on, 4) &&
                host.write_memory(host.userdata, menu + 0x2304, &target, 4))
                walk_wait = now + 3;
        }
        if (walk_to >= 0) {
            if (now > walk_until) {
                walk_to = -1;
            } else if (static_cast<int>(cursor) == walk_to) {
                pushed_at[cursor] = now;
                Press("a");
                walk_to = -1;
                files_boost = !files_boost;
            } else if (walk_wait > now) {
            } else if (static_cast<int>(cursor) != walk_to) {
                Press(static_cast<int>(cursor) < walk_to ? "down" : "up");
                walk_wait = now + 10;
            }
        }
        // the host republishes the page at 30 Hz unless one of its own animations runs: flipping
        // this gate starts an invisible group animation, so the short moves are drawn at 60 Hz
        host.publish_i64(host.userdata, "files.boost", files_boost ? 1 : 0);
        for (int i = 0; i < 3; ++i) {
            const std::string p = "files." + std::to_string(i) + ".";
            const bool focused = static_cast<int>(cursor) == i;
            const float t = static_cast<float>(now - moved_at[i]), pt = static_cast<float>(now - pushed_at[i]);
            const bool pushing = pt < 10;
            float scale = focused ? (t < 6 ? Curve(BtnFocus, t) : 1) : (t < 4 ? Curve(BtnUnfocus, t) : 0.92f);
            float outline = focused ? (t < 6 ? Curve(CursorFocus, t) : Curve(CursorWait, std::fmod(t - 6, 60.0f)))
                                    : (t < 4 ? Curve(CursorUnfocus, t) : 0);
            float burst = 1, flash = 0;
            if (pushing) {
                scale = pt < 8 ? Curve(BtnPush, pt) : 1;
                flash = pt < 8 ? Curve(BtnFlash, pt) : 0;
                outline = Curve(CursorPushAlpha, pt);
                burst = (Curve(CursorPushX, pt) + Curve(CursorPushY, pt)) / 2;
            }
            host.publish_i64(host.userdata, (p + "scale").c_str(), std::lround(scale * 1000));
            host.publish_i64(host.userdata, (p + "outline").c_str(), std::clamp<long>(std::lround(outline * 31 / 255), 0, 31));
            host.publish_i64(host.userdata, (p + "outline_scale").c_str(), std::lround(scale * burst * 1000));
            host.publish_i64(host.userdata, (p + "flash").c_str(), std::clamp<long>(std::lround(flash * 15 / 50), 0, 15));
            host.publish_i64(host.userdata, (p + "dim").c_str(), focused || pushing ? 0 : 1);
        }
    }

    void FilesStep(const EdenDsmodHostApi& host, std::optional<u64> menu, int screen) {
        // keys are known from the title on (the buttons exist with the title menu), so the pictures
        // are drawn before the file select opens; the page waits for files.ready
        bool ready = screen == 3;
        if (screen >= 1 && menu && host.publish_text) {
            ++files_frame;
            u32 cursor{};
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *menu + 0x2304, &cursor, 4);
            // back from a game the menu's cursor is -1: the mode select keeps showing the file last chosen
            if (cursor <= 2)
                files_cursor = cursor;
            const auto text = [&](const std::string& name, const std::string& key, bool needed = true) {
                const bool drawn = FilesReady(key, screen == 3);
                ready = (drawn || !needed) && ready;
                if (drawn)
                    host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + key).c_str());
            };
            ready = FilesReady("cursor") && FilesReady("dim") && FilesReady("flash") && ready;
            if (rainbow_slot >= 0 && cursor != static_cast<u32>(rainbow_slot))
                rainbow_slot = -1;
            PrewarmFiles(host);
            FilesMotion(host, *menu, cursor, screen == 3);
            for (int i = 0; i < 3; ++i) {
                const std::string p = "files." + std::to_string(i) + ".";
                u64 pane{};
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *menu + 0x2048 + i * 0x138, &pane, 8);
                const auto pct = SlotPercent(host, pane);
                if (!pct) {
                    ready = false;
                    continue;
                }
                // a deleted file keeps its old number: the game shows N_NoData instead of it
                const bool empty = pct->empty() || SlotPaneShown(host, nodata_pane, i, pane, "N_NoData", false);
                const bool complete = !empty && SlotPaneShown(host, comp_pane, i, pane, "N_CompBtn", true);
                const auto flags = SlotFlags(host, i);
                // header byte 15: Magoland open (byte 0: started); the story-clear bytes are still unconfirmed
                const bool magoland = complete || (flags && (*flags)[15] != 0);
                if (files_frame % 30 == 1 || !progress[i])
                    progress[i] = SlotProgress(host, i);
                const int face = empty ? 1 : complete ? 4 : progress[i] ? progress[i]->face : 2;
                const std::string look = complete ? "c" : empty ? "d" : "n";
                const bool focused = cursor == static_cast<u32>(i);
                const std::string shown_pct = empty ? "-" : std::string(pct->begin(), pct->end());
                host.publish_i64(host.userdata, (p + "complete").c_str(), complete ? 1 : 0);
                host.publish_i64(host.userdata, (p + "focus").c_str(), cursor == static_cast<u32>(i) ? 1 : 0);
                text(p + "panel", "panel/" + look + "/" + std::to_string(i) + (complete ? focused ? "/1000" : "/920" : ""));
                text(p + "kirby", "kirby/" + std::to_string(face) + "/" + (complete ? "1" : "0"));
                if (!title_dir.empty())
                    text(p + "label", "label/" + title_dir + "/" + std::to_string(i + 1));
                text(p + "pct", "pct/" + shown_pct);
                text(p + "modes", "modes/" + look + "/" + (empty ? "1" : "0") + (magoland ? "00" : "11"));
                std::string main_pct = "-", magoland_pct = "-";
                if (!empty && progress[i]) {
                    main_pct = std::to_string(progress[i]->main);
                    if (progress[i]->magoland >= 0)
                        magoland_pct = std::to_string(progress[i]->magoland);
                }
                text(p + "modepct", "modepct/" + look + "/" + main_pct + "/" + magoland_pct);
                // the colours run one panel width per 180 frames, from the start each time the cursor lands
                const bool sliding = complete && focused;
                if (sliding && rainbow_slot != i)
                    rainbow_slot = i, rainbow_start = files_frame;
                const auto step = sliding ? (files_frame - rainbow_start) * RainbowSteps / RainbowFrames % RainbowSteps : 0;
                text(p + "rainbow", "rainbow/" + std::to_string(step), step == 0);
            }
        }
        // the main mode's own menu, told by its music; Past Adventures stays in it, playing each past game's music (e.g.
        // Bln_Glassland1) with the cursor on its button
        const bool main_menu = screen == 4 && menu && MainModeMenu(host, *menu);
        u32 cursor = ~u32{0};
        const std::string music = menu ? MenuMusic(host, *menu) : "";
        past_adventures = screen == 4 && menu && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *menu + 0x3EF4, &cursor, 4) &&
                          cursor == 6 && !music.empty() && !music.starts_with("BGM_") && music != "_NO_SOUND";
        const bool modes = screen >= 3 && menu && host.publish_text && ModesStep(host, files_cursor, screen);
        host.publish_i64(host.userdata, "modes.ready", modes && !main_menu && !past_adventures ? 1 : 0);
        // the menu's page waits for the story's window, so it shows at once instead of sliding in late
        host.publish_i64(host.userdata, "modes.main",
                         main_menu && modes_ok && FilesReady("mainmenu", true) && !adventure[0].panel.empty() ? 1 : 0);
        // the main mode's menu cursor (M+0x3EF4): 0 story, 7 extra, then 1 Arena, 2 Magolor Epilogue,
        // 3 theater, 4 jukebox, 6 previous adventures
        u32 button = ~u32{0};
        if (main_menu && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *menu + 0x3EF4, &button, 4))
            menu_button = button;
        SlideStep(host, main_menu, button);
        if (host.log && (screen == 3) != files_was_on) {
            files_was_on = screen == 3;
            files_opened = files_frame;
        }
        if (host.log && ready && !files_was_ready) {
            const std::string msg = "Kirby DSMod files page ready " + std::to_string(files_frame - files_opened) +
                                    " frame(s) after the file select opened";
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
        }
        files_was_ready = ready;
        host.publish_i64(host.userdata, "files.on", screen == 3 ? 1 : 0);
        host.publish_i64(host.userdata, "files.ready", ready ? 1 : 0);
    }

    // Title menu object M = [[[main+0x16325B8]+0x1930]+0x258], recognised by its first words
    // (10, 15) and its MainTitle layout [M+0xFC0] (vtable main+0x15A18D0). M+0x4D0: 0 attract
    // demo, 1 title, 2 after the first A. M+0x13A0: 0 title, 2 file/mode select, 3 back on the
    // title. M+0x1EC8: file select phase (0 = not shown). The webm attract movie runs while
    // main+0x1626C30 is 1, with M gone.
    // Layout panes: name at +0xB0, scale x at +0x48, alpha at +0x59.
    std::string title_dir;
    std::array<s32, 1> pane_slot{-1}; // N_Start's offset inside the layout
    int title_grace{}; // frames the title page outlives the menu (movie hand-overs leave a gap)
    bool from_game{};  // a game runs, or the menu came back from one (not from the attract movie)
    int menu_screen{}; // the last screen while the menu existed
    bool modes_ok{};   // the mode select may show: reached from the file select or a game
    u64 title_frame{}, title_push{~u64{0} >> 1};

    std::optional<u64> TitleMenu(const EdenDsmodHostApi& host) const {
        u64 root{}, scope{}, menu{}, layout{}, vt{};
        u32 head[2]{};
        const auto rd = [&](u64 at, void* out, std::size_t n) {
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, n);
        };
        if (!rd(host.main_base + 0x16325B8, &root, 8) || !rd(root + 0x1930, &scope, 8) ||
            !rd(scope + 0x258, &menu, 8) || !rd(menu, head, 8) || head[0] != 10 || head[1] != 15 ||
            !rd(menu + 0xFC0, &layout, 8) || !rd(layout, &vt, 8) || vt != host.main_base + 0x15A18D0)
            return std::nullopt;
        return menu;
    }

    // The layout object holds its panes inline; a pane is found by its name and cached by offset.
    std::optional<u64> TitlePane(const EdenDsmodHostApi& host, u64 layout, int which) {
        static constexpr std::string_view Names[]{"N_Start"};
        static constexpr u32 Span = 0x4000, NameAt = 0xB0;
        const auto named = [&](u64 pane) {
            char name[16]{};
            u64 vt{};
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + NameAt, name, 15) &&
                   std::string_view{name} == Names[which] &&
                   dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane, &vt, 8) &&
                   vt >= host.main_base && vt < host.main_base + host.main_size;
        };
        if (pane_slot[which] >= 0 && named(layout + pane_slot[which]))
            return layout + pane_slot[which];
        std::vector<char> bytes(Span);
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout, bytes.data(), Span))
            return std::nullopt;
        const std::string_view all{bytes.data(), bytes.size()};
        const std::string wanted = std::string{Names[which]} + '\0';
        for (auto at = all.find(wanted); at != std::string_view::npos; at = all.find(wanted, at + 1))
            if (at >= NameAt && named(layout + at - NameAt)) {
                pane_slot[which] = static_cast<s32>(at - NameAt);
                return layout + pane_slot[which];
            }
        return std::nullopt;
    }

    // The world map's world selection. The running scene [main+0x1642EE8] holds the map's UI at +0xBA8
    // (vtable main+0x159D630) only there, not in a world's hub, a stage or the menus; +0xB48 is the
    // planet Kirby is on (0 Pop Star, 1 Halcandra), +0x3C8 the marker he stands on (Pop Star worlds 1-5
    // 0-4, Lor 5; Halcandra world 6 8, world 7 9, Lor 10) and +0xC30 is 2 while he is on the marker
    // that flies to the other planet (+0x3C8 keeps the Lor's). While a game runs,
    // [main+0x16325B8]+0x268 is its mode (0 story, 1 extra), +0x27C the lives and +0x280 the point
    // stars. The file is the one last chosen on the mode select. The page shows the last picture
    // drawn, never one still missing, and crossfades to the other planet's.
    static constexpr int Travel = 7; // the marker to the other planet, in the walk's terms
    struct MapState {
        bool halcandra{};
        int at{}; // marker, Travel on the travel marker
    };
    int wmap_quiet{}; // ticks since the title menu was last there
    u64 wmap_tick{}, wmap_focus{};
    std::optional<Progress> wmap_progress;
    std::array<std::string, 2> wmap_page{};
    std::array<int, 2> wmap_planet{-1, -1};
    int wmap_set{}, wmap_cursor{-1};
    bool wmap_live{}; // the world map page shows
    // per icon: when the cursor left it, when it was tapped (none yet)
    static constexpr auto Never = [] {
        std::array<u64, KirbyModes::MapSlots.size()> a;
        a.fill(~u64{0} >> 1);
        return a;
    }();
    std::array<u64, KirbyModes::MapSlots.size()> wmap_left = Never, wmap_pushed = Never;

    // Going to a tapped icon: at once (see WalkStep), or from the travel marker one D-pad step at a time
    // along the map's paths, each pressed again until Kirby stands on the next marker.
    struct Walk {
        int target{-1};
        int from{-1};      // the marker the last press left from
        u64 pressed{}, arrived{}, started{};
        int presses{};
    } walk;

    static std::optional<MapState> MapRead(const EdenDsmodHostApi& host, u64 scene) {
        u32 planet{}, marker{}, travel{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, scene + 0xB48, &planet, 4) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, scene + 0x3C8, &marker, 4) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, scene + 0xC30, &travel, 4) || planet > 1)
            return std::nullopt;
        return MapState{planet == 1, travel == 2 ? Travel : static_cast<int>(marker)};
    }

    // The paths between markers and the D-pad direction of each (validated on both planets); the
    // walk only crosses open worlds.
    static std::vector<std::pair<int, const char*>> MapLinks(bool halcandra, int at) {
        if (halcandra)
            switch (at) {
            case 10: return {{8, "map_right"}, {Travel, "map_left"}};
            case 8: return {{9, "map_up"}, {10, "map_left"}};
            case 9: return {{8, "map_down"}};
            case Travel: return {{10, "map_right"}};
            default: return {};
            }
        switch (at) {
        case 5: return {{0, "map_up"}, {Travel, "map_down"}};
        case 0: return {{1, "map_right"}, {4, "map_left"}, {5, "map_down"}};
        case 1: return {{2, "map_down"}, {0, "map_left"}};
        case 2: return {{3, "map_left"}, {1, "map_up"}};
        case 3: return {{4, "map_up"}, {2, "map_right"}};
        case 4: return {{3, "map_down"}, {0, "map_right"}};
        case Travel: return {{5, "map_up"}};
        default: return {};
        }
    }

    // the first step from `at` towards `target` and the marker it reaches
    static std::optional<std::pair<int, const char*>> MapStep(bool halcandra, int at, int target, int open) {
        const auto usable = [&](int m) {
            const int world = halcandra ? (m == 8 ? 5 : m == 9 ? 6 : -1) : (m <= 4 ? m : -1);
            return world < 0 || world < open;
        };
        std::map<int, std::pair<int, const char*>> first; // marker -> first step taken to reach it
        std::vector<int> queue{at};
        first[at] = {at, nullptr};
        for (std::size_t i = 0; i < queue.size(); ++i)
            for (const auto& [next, dir] : MapLinks(halcandra, queue[i]))
                if (usable(next) && !first.contains(next)) {
                    first[next] = queue[i] == at ? std::pair{next, dir} : first[queue[i]];
                    if (next == target)
                        return first[next];
                    queue.push_back(next);
                }
        return std::nullopt;
    }

    static int MarkerOf(const KirbyModes::MapSlot& slot) {
        if (slot.world == 7)
            return Travel;
        if (slot.world < 0)
            return slot.halcandra ? 10 : 5;
        return slot.halcandra ? slot.world + 3 : slot.world;
    }

    // A tap on an icon. The game takes the marker Kirby stands on from [main+0x16325B8]+0x1C17C (and
    // +0x1C180 beside it), the scene's +0x3C8 being a copy: written to the one tapped, the next A enters
    // it at once, from wherever he stands (validated on both planets). The marker that flies to the
    // other planet is 6 / 11 there (the scene keeps the Lor's): written there, A flies off at once,
    // without the walk to it. Only from that marker, where A would fly off, he first steps to the Lor
    // to enter a world.
    void WalkStep(const EdenDsmodHostApi& host, const MapState& map, int open, u64 scene, u64 root) {
        if (const int tap = wmap_tap.exchange(-1); tap >= 0 && tap < static_cast<int>(KirbyModes::MapSlots.size()) &&
                                                   (KirbyModes::MapSlots[tap].halcandra == map.halcandra ||
                                                    KirbyModes::MapSlots[tap].world == 7)) {
            walk = {MarkerOf(KirbyModes::MapSlots[tap]), map.at, 0, wmap_tick - 12, wmap_tick, 0};
            wmap_pushed[tap] = wmap_tick;
        }
        if (walk.target < 0)
            return;
        if (map.at != walk.from)
            walk.from = map.at, walk.arrived = wmap_tick, walk.presses = 0;
        // give up after ten seconds, or after pressing the same step (or A) four times in vain
        if (wmap_tick - walk.started > 600 || walk.presses >= 4) {
            walk.target = -1;
            return;
        }
        // let him settle on a marker before the next step, and give each press time to take
        if (wmap_tick - walk.arrived < 12 || (walk.presses > 0 && wmap_tick - walk.pressed < 45))
            return;
        const u32 target = static_cast<u32>(walk.target);
        // the game's own id of the marker that flies off: 6 on Pop Star, 11 on Halcandra
        const u32 travel = map.halcandra ? 11 : 6;
        if (map.at == walk.target) {
            Press("a");
        } else if (walk.target == Travel && host.write_memory && host.write_memory(host.userdata, root + 0x1C17C, &travel, 4) &&
                   host.write_memory(host.userdata, root + 0x1C180, &travel, 4)) {
            Press("a");
        } else if (walk.target != Travel && map.at != Travel && host.write_memory &&
                   host.write_memory(host.userdata, root + 0x1C17C, &target, 4) &&
                   host.write_memory(host.userdata, root + 0x1C180, &target, 4) &&
                   host.write_memory(host.userdata, scene + 0x3C8, &target, 4)) {
            Press("a");
        } else if (const auto step = MapStep(map.halcandra, map.at, walk.target, open)) {
            Press(step->second);
        } else {
            walk.target = -1;
            return;
        }
        // the flight moves Kirby onto the travel marker: another A there, mid-flight, hangs the game
        if (walk.target == Travel)
            walk.target = -1;
        walk.pressed = wmap_tick;
        ++walk.presses;
    }

    std::string WorldMapKey(int planet, int mode, int lives, int stars, int spheres) const {
        return "wmap/" + title_dir + "/" + std::to_string(planet) + "/" + std::to_string(mode) + "/" + std::to_string(lives) + "." +
               std::to_string(stars) + "." + std::to_string(spheres);
    }

    void WorldMapStep(const EdenDsmodHostApi& host, bool menu) {
        const auto rd = [&](u64 at, auto& out) {
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out);
        };
        ++wmap_tick;
        wmap_quiet = menu ? 0 : std::min(wmap_quiet + 1, 1000);
        u64 scene{}, ui{}, vt{}, root{};
        u32 mode{}, lives{}, stars{};
        std::optional<MapState> map;
        if (!menu && rd(host.main_base + 0x1642EE8, scene) && scene && rd(scene + 0xBA8, ui) && ui && rd(ui, vt) &&
            vt == host.main_base + 0x159D630 && rd(host.main_base + 0x16325B8, root) && rd(root + 0x268, mode) && mode <= 1 &&
            rd(root + 0x27C, lives) && rd(root + 0x280, stars))
            map = MapRead(host, scene);
        if (map)
            play_mode = static_cast<int>(mode);
        if (!map || files_cursor > 2 || title_dir.empty() || !host.publish_text) {
            wmap_progress.reset();
            walk.target = -1;
            wmap_tap = -1;
            wmap_live = false;
            // the page stays as it was while the game loads what Kirby entered (the host drops what is no
            // longer published), until another page takes over or the title menu is back
            if (menu)
                wmap_held.clear();
            for (const auto& [name, v] : wmap_held)
                if (v.text.empty())
                    host.publish_i64(host.userdata, name.c_str(), v.value);
                else
                    host.publish_text(host.userdata, name.c_str(), v.text.c_str());
            host.publish_i64(host.userdata, "wmap.on", 0);
            return;
        }
        if (map->at <= 4 || map->at == 8 || map->at == 9)
            play_world = map->at <= 4 ? map->at : map->at - 3;
        else
            play_world = -1;
        const auto pub = [&](const std::string& name, std::int64_t v) {
            host.publish_i64(host.userdata, name.c_str(), v);
            wmap_held[name] = {v, {}};
        };
        const auto pub_text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, name.c_str(), v.c_str());
            wmap_held[name] = {0, v};
        };
        if (!wmap_progress || wmap_tick % 60 == 0)
            if (auto p = SlotProgress(host, static_cast<int>(files_cursor)))
                wmap_progress = std::move(p);
        const int open = wmap_progress ? wmap_progress->worlds[mode].open : 0;
        const bool other = map->halcandra || open >= 6;
        if (wmap_progress) {
            const auto& m = mode ? wmap_progress->extra : wmap_progress->story;
            const int planet = map->halcandra ? 1 : 0;
            const std::string key = WorldMapKey(planet, static_cast<int>(mode), static_cast<int>(std::min(lives, 999u)),
                                                static_cast<int>(std::min(stars, 9999u)), m.spheres);
            if (key != wmap_page[wmap_set] && FilesReady(key)) {
                // another planet fades in over the one shown; anything else just takes its place
                const bool fade = !wmap_page[wmap_set].empty() && wmap_planet[wmap_set] != planet;
                if (fade)
                    wmap_set ^= 1;
                if (!wmap_page[wmap_set].empty() && wmap_page[wmap_set] != wmap_page[wmap_set ^ 1])
                    FilesForget(wmap_page[wmap_set]);
                wmap_page[wmap_set] = key;
                wmap_planet[wmap_set] = planet;
            }
        }
        WalkStep(host, *map, open, scene, root);
        // the stage page's bar as it will first show (no spheres yet), drawn while the map is up
        FilesAhead(PlayBarKey(host, static_cast<int>(std::min(lives, 999u)), static_cast<int>(std::min(stars, 9999u)), 0, 0, -1, false));
        // the cursor on the icon of the marker under Kirby: fades in, then pulses like the menus' cursors
        int cursor = -1;
        for (std::size_t i = 0; i < KirbyModes::MapSlots.size(); ++i) {
            const auto& slot = KirbyModes::MapSlots[i];
            if (MarkerOf(slot) == map->at && (slot.world == 7 ? other : slot.halcandra == map->halcandra))
                cursor = static_cast<int>(i);
        }
        if (cursor != wmap_cursor) {
            if (wmap_cursor >= 0)
                wmap_left[wmap_cursor] = wmap_tick;
            wmap_cursor = cursor, wmap_focus = wmap_tick;
        }
        const float f = static_cast<float>(wmap_tick - wmap_focus);
        pub("wmap.pulse", std::lround((f < 6 ? f / 6 : 0.5f + 0.5f * std::cos((f - 6) * 6.2831853f / 60)) * 15));
        for (std::size_t i = 0; i < KirbyModes::MapSlots.size(); ++i) {
            const auto& slot = KirbyModes::MapSlots[i];
            const std::string n = std::to_string(i);
            const bool usable = slot.world == 7 ? other : slot.world < 0 || slot.world < open;
            // ModeSelectBtn*_Focus (1 to 1.12 to 1.08 in 6 frames), back to 1 in 4 once the cursor
            // leaves, and _Push (down to 0.92 and back in 8) for a tap
            float scale = 1;
            if (static_cast<int>(i) == cursor)
                scale = f < 2 ? 1 + 0.06f * f : f < 6 ? 1.12f - 0.01f * (f - 2) : 1.08f;
            else if (const float l = static_cast<float>(wmap_tick - wmap_left[i]); l < 4)
                scale = 1.08f - 0.02f * l;
            if (const float q = static_cast<float>(wmap_tick - wmap_pushed[i]); wmap_tick >= wmap_pushed[i] && q < 8)
                scale *= q < 3 ? 1 - 0.148f * q / 3 : (0.92f + 0.16f * (q - 3) / 5) / 1.08f;
            pub("wmap.scale." + n, std::lround(scale * 1000));
            pub("wmap.cur." + n, static_cast<int>(i) == cursor && usable ? 1 : 0);
            // each crossfading set shows its own planet's icons; the touch areas follow the planet Kirby is on
            for (int set = 0; set < 2; ++set)
                pub(std::string{"wmap."} + "ab"[set] + ".icon." + n,
                    slot.world != 7 && usable && wmap_planet[set] == (slot.halcandra ? 1 : 0) ? 1 : 0);
            pub("wmap.icon." + n, usable && (slot.world == 7 || slot.halcandra == map->halcandra) ? 1 : 0);
        }
        pub_text("wmap.other", map->halcandra ? "module:kirby:gen/files/wother/0" : "module:kirby:gen/files/wother/1");
        for (int s = 0; s < 2; ++s) {
            const std::string p = std::string{"wmap."} + "ab"[s] + ".";
            if (!wmap_page[s].empty())
                pub_text(p + "page", "module:kirby:gen/files/" + wmap_page[s]);
            pub(p + "open", s == wmap_set && !wmap_page[s].empty() ? 1 : 0);
        }
        // the menu's own page switch wins: the map page waits a few ticks after the menu is gone
        wmap_live = !wmap_page[wmap_set].empty() && wmap_quiet > 2;
        host.publish_i64(host.userdata, "wmap.on", wmap_live ? 1 : 0);
    }

    // Magolor Epilogue's page texts: the game's own (Mahoroa.msbt: the techniques' names, "Level") and ours
    struct EpiWords {
        std::u32string level, to, max, magic;
        std::array<std::u32string, 4> moves;  // Magolor Cannon, Black Hole, then ours: Heal, Magolor Blast
        std::array<std::u32string, 2> learnt, info; // the popups of Heal and Magolor Blast ([highlights])
        std::u32string ok;
    };
    EpiWords EpiTexts(const std::string& dir) {
        const auto m = romfs("/msg/Ark/" + dir + "/Mahoroa.msbt");
        const auto msg = [&](const char* label, const char32_t* fallback) {
            const auto t = m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
            return t && !t->empty() ? *t : std::u32string{fallback};
        };
        struct Own {
            const char* lang;
            const char32_t *to, *magic, *heal, *learnt, *heal_info, *ok, *blast, *blast_learnt, *blast_info;
        };
        static constexpr Own Ours[]{
            {"Italian", U"al", U"Magie", U"Cura", U"Hai sbloccato la magia \"[Cura]\"!",
             U"Riempi al massimo la barra dell'energia magica\nper [curare metà della tua vita].", U"Capito",
             U"Esplosione Magolor", U"Hai sbloccato la magia \"[Esplosione Magolor]\"!",
             U"Riempi al massimo la barra dell'energia magica\nper [colpire tutti i nemici sullo schermo]."},
            {"English", U"to", U"Magic", U"Heal", U"You've unlocked the \"[Heal]\" magic!",
             U"Fill the magic energy gauge all the way\nto [restore half of your health].", U"Got it",
             U"Magolor Blast", U"You've unlocked the \"[Magolor Blast]\" magic!",
             U"Fill the magic energy gauge all the way\nto [hit every enemy on the screen]."},
            {"French", U"au", U"Magie", U"Soin", U"Tu as débloqué la magie « [Soin] » !",
             U"Remplis entièrement la jauge d'énergie magique\npour [récupérer la moitié de ta santé].", U"Compris",
             U"Explosion Magolor", U"Tu as débloqué la magie « [Explosion Magolor] » !",
             U"Remplis entièrement la jauge d'énergie magique\npour [frapper tous les ennemis à l'écran]."},
            {"German", U"bis", U"Magie", U"Heilen", U"Du hast den Zauber „[Heilen]“ freigeschaltet!",
             U"Fülle die Leiste der magischen Energie ganz auf,\num [die Hälfte deiner Gesundheit zu heilen].", U"Verstanden",
             U"Magolor-Explosion", U"Du hast den Zauber „[Magolor-Explosion]“ freigeschaltet!",
             U"Fülle die Leiste der magischen Energie ganz auf,\num [alle Gegner auf dem Bildschirm zu treffen]."},
            {"Spanish", U"al", U"Magia", U"Curar", U"¡Has desbloqueado la magia \"[Curar]\"!",
             U"Llena por completo la barra de energía mágica\npara [recuperar la mitad de tu salud].", U"Entendido",
             U"Explosión Magolor", U"¡Has desbloqueado la magia \"[Explosión Magolor]\"!",
             U"Llena por completo la barra de energía mágica\npara [golpear a todos los enemigos en pantalla]."},
            {"Dutch", U"tot", U"Magie", U"Genezen", U"Je hebt de magie \"[Genezen]\" ontgrendeld!",
             U"Vul de balk met magische energie helemaal\nom [de helft van je gezondheid te herstellen].", U"Begrepen",
             U"Magolor-explosie", U"Je hebt de magie \"[Magolor-explosie]\" ontgrendeld!",
             U"Vul de balk met magische energie helemaal\nom [alle vijanden op het scherm te raken]."},
            {"Portuguese", U"ao", U"Magia", U"Curar", U"Você desbloqueou a magia \"[Curar]\"!",
             U"Encha totalmente a barra de energia mágica\npara [recuperar metade da sua saúde].", U"Entendi",
             U"Explosão Magolor", U"Você desbloqueou a magia \"[Explosão Magolor]\"!",
             U"Encha totalmente a barra de energia mágica\npara [atingir todos os inimigos na tela]."},
            {"Japanese", U"→", U"まほう", U"かいふく", U"まほう「[かいふく]」をおぼえた！",
             U"まほうエネルギーのゲージをいっぱいにすると\n[たいりょくがはんぶんかいふく]するよ！", U"わかった",
             U"マホロアバースト", U"まほう「[マホロアバースト]」をおぼえた！",
             U"まほうエネルギーのゲージをいっぱいにすると\n[がめんのてきみんなにこうげき]できるよ！"},
            {"Korean", U"→", U"마법", U"회복", U"마법 '[회복]'을 익혔다!", U"마법 에너지 게이지를 가득 채우면\n[체력의 절반을 회복]할 수 있어요!",
             U"알겠어요", U"마호로아 버스트", U"마법 '[마호로아 버스트]'를 익혔다!",
             U"마법 에너지 게이지를 가득 채우면\n[화면의 모든 적을 공격]할 수 있어요!"},
            {"CN_Chinese", U"→", U"魔法", U"治疗", U"解锁了魔法「[治疗]」！", U"将魔法能量槽完全充满，\n就能[恢复一半体力]！", U"知道了",
             U"魔法洛亚爆发", U"解锁了魔法「[魔法洛亚爆发]」！", U"将魔法能量槽完全充满，\n就能[攻击画面上的所有敌人]！"},
            {"TW_Chinese", U"→", U"魔法", U"治療", U"解鎖了魔法「[治療]」！", U"將魔法能量槽完全充滿，\n就能[恢復一半體力]！", U"知道了",
             U"魔法洛亞爆發", U"解鎖了魔法「[魔法洛亞爆發]」！", U"將魔法能量槽完全充滿，\n就能[攻擊畫面上的所有敵人]！"}};
        const Own* own = &Ours[1];
        for (const auto& o : Ours)
            if (dir.find(o.lang) != std::string::npos)
                own = &o;
        return {msg("EnhancingMenu_CurrentLevelMax", U"Level"), own->to, U"MAX", own->magic,
                {msg("SkillName_MahoroaCannon", U"Magolor Cannon"), msg("SkillName_BlackHole", U"Black Hole"), own->heal, own->blast},
                {own->learnt, own->blast_learnt}, {own->heal_info, own->blast_info}, own->ok};
    }

    // The stage page's texts: the game's own where it has them (Step.msbt ChargeInfo_DiscardAbility,
    // Option.msbt TopTitle), ours for the rest, by the game's message directory.
    struct PlayLabels {
        std::u32string pause, discard, game, abilities, options, resume;
    };
    PlayLabels PlayTexts(const std::string& dir) {
        const auto msg = [&](const char* file, const char* label) {
            const auto m = romfs("/msg/Ark/" + dir + "/" + file + ".msbt");
            const auto t = m ? KirbyAssets::MsbtText(*m, label) : std::nullopt;
            return t ? *t : std::u32string{};
        };
        struct Own {
            const char* lang;
            const char32_t *pause, *game, *abilities;
        };
        static constexpr Own Ours[]{{"Italian", U"Pausa", U"Partita", U"Abilità"},
                                    {"English", U"Pause", U"Game", U"Abilities"},
                                    {"French", U"Pause", U"Partie", U"Pouvoirs"},
                                    {"German", U"Pause", U"Spiel", U"Fähigkeiten"},
                                    {"Spanish", U"Pausa", U"Partida", U"Habilidades"},
                                    {"Dutch", U"Pauze", U"Spel", U"Krachten"},
                                    {"Portuguese", U"Pausa", U"Partida", U"Habilidades"},
                                    {"Japanese", U"ポーズ", U"ゲーム", U"のうりょく"},
                                    {"Korean", U"일시 정지", U"게임", U"능력"},
                                    {"CN_Chinese", U"暂停", U"游戏", U"能力"},
                                    {"TW_Chinese", U"暫停", U"遊戲", U"能力"}};
        const Own* own = &Ours[1];
        for (const auto& o : Ours)
            if (dir.find(o.lang) != std::string::npos)
                own = &o;
        return {own->pause, msg("Step", "ChargeInfo_DiscardAbility"), own->game, own->abilities, msg("Option", "TopTitle"),
                msg("YyCmn", "InGamePause_Pause_Continue")};
    }

    // The stage page. Kirby can act unless a menu is open: [main+0x16325B8]+0x378 is 0 in play, 1 in
    // the pause menu (the amiibo dialog included), 7 in the souvenir menu. The stage HUD (Step/Parts SphereBgStep, laid out inline in
    // [[[root+0x148]+0xB8]+0x70]) tells the stage's energy spheres: N_SpherePlacement shows in a stage
    // only, L_Sphere<n> per sphere, its P_SphereON02 once it is got. Souvenirs come from the file's live
    // save block (the second of its pair), the mode from the world map.
    int play_mode{};
    bool play_loading{}, play_between{}, play_gone{};
    int play_white{};
    u32 menu_button = ~u32{0}; // the main mode's menu cursor when last seen: 2 enters Magolor Epilogue, 1 the Arena, 8 the True Arena
    bool past_adventures{};
    // The Arenas' page: the stage page's controls over the two figures. arena_boss is the boss shown (its folder
    // in gfx/Step/Chara, its name Step.msbt Chara<name>), arena_time the time so far in centiseconds (-1 unknown).
    int arena_mode{}; // 1 Arena, 2 True Arena
    u32 play_ability{};
    std::string arena_boss, arena_boss_name, arena_card, arena_card_name; // the boss wanted; the boss the card shows
    std::string arena_character{"Kirby"};                                  // the player's plate
    bool arena_rest{}, arena_was_on{};                                     // in a rest room (its board read); in play last tick
    std::map<std::string, std::string> arena_shown;                        // the figures' and the boss plate's last pictures
    std::string arena_figure;                                              // the player's figure's frames (their key less the frame)
    u64 arena_plain_since{};                                               // since when the ability reads as none
    enum VsState { VsNone, VsPlaying, VsFading, VsClearing };
    VsState vs_state{VsNone};
    u64 vs_at{}, vs_end{}, vs_gone{}, arena_door_at{};
    bool arena_card_on{};
    u64 arena_card_off{};
    static constexpr u64 ArenaCardTicks = 20; // the card's slide (ARENA_CARD_MS), and a little more
    long arena_time{-1};
    Swap arena_bar;
    int play_world{-1}; // the world Kirby went into from the map (0..6), -1 the Lor
    int play_boss{-1};  // the world whose boss's room this is (until the room changes)
    struct Held {
        std::int64_t value{};
        std::string text;
    };
    std::map<std::string, Held> wmap_held; // the world map page as last published
    u64 play_tick{}, play_layout{};
    std::array<u64, KirbyModes::PlayButtons> play_pushed = [] {
        std::array<u64, KirbyModes::PlayButtons> a;
        a.fill(~u64{0} >> 1);
        return a;
    }();
    std::vector<u32> play_panes; // N_SpherePlacement, then per sphere L_Sphere<n> and its P_SphereON02
    int play_spheres{};
    unsigned play_got{};
    bool play_hud{}, play_stage{}; // the stage HUD exists (a hub or a stage); it is a stage
    Swap play_bar, play_souv;
    // The tabs (0 the game's controls, 1 the copy ability's pause pages, 2 options) and the Abilities tab:
    // its pages slide by the arrows, the sections slide across when the tab changes.
    static constexpr int SectionFrames = 10, PageFrames = 12, TabFrames = 6;
    static constexpr float SectionShift = 1240, PageShift = 1100, TabDrop = 12;
    int play_tab{}, tab_from{};
    u64 tab_at{};
    bool play_boost{};
    // A section's pages of moves (the Abilities tab, the Epilogue's Magic tab): an arrow slides the page shown
    // away from it, the next (right) or the previous (left) one comes in; back to the first when what they
    // show changes
    struct Pager {
        std::atomic<int> tap{-1};
        std::string content;
        int page{}, set{}, dir{};
        u64 at{};
        std::array<std::string, 2> key;
        Swap info, pill;
        std::array<u64, 2> pushed{~u64{0} >> 1, ~u64{0} >> 1};
    };
    Pager abl;
    int abl_id{-1};
    std::mutex abl_mutex;
    std::map<std::string, std::shared_ptr<const KirbyModes::AbilityPages>> abl_content;
    // An Options tab (the stage page's, the Epilogue's): the list (view 0), the HUD page (1) and the cheats page (2),
    // which slide in over it
    struct OptionsUi {
        std::atomic<int> tap{-1};
        int view{}, view_from{}, panel{1};
        u64 view_at{};
        std::array<u64, 10> pushed = [] {
            std::array<u64, 10> a;
            a.fill(~u64{0} >> 1);
            return a;
        }();
        Swap row0, card;
        std::array<Swap, 4> lines;
    };
    OptionsUi opt, epo;
    // the cheat's choice: Kirby's copy abilities (Landia, 27, left out) and the five super abilities
    static constexpr std::array<int, 31> CheatAbilities{1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                                        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 28, 29, 30, 31, 32};
    std::size_t cheat_index{1};
    int cheat_pending{}, cheat_tries{};
    // The cheat: Kirby's message queue Q = [P+0x260] (u32 count at +0x10, then 0x14-byte messages: type,
    // a, b, c, d) is read by his handler (main+0xEE21F0) every update. Type 19 copies ability a (0x27: the
    // Ability Star souvenir's random one, in b) through the game's own code (main+0xECAB0C), replacing the
    // one he has (validated 2026-10-18 under the debugger). Appended only to an empty queue, count last.
    void CheatStep(const EdenDsmodHostApi& host) {
        if (!cheat_pending || !host.write_memory)
            return;
        u64 root{}, rec{}, p{}, q{};
        u32 count = 1;
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (rd(host.main_base + 0x16325B8, root) && rd(root + 0x260, rec) && rd(rec + 0x140, p) && rd(p + 0x260, q) && rd(q + 0x10, count) &&
            count == 0 && !play_posing) {
            const u32 message[5]{19, static_cast<u32>(cheat_pending), 0, 1, 0};
            const u32 one = 1;
            host.write_memory(host.userdata, q + 0x14, message, sizeof message);
            host.write_memory(host.userdata, q + 0x10, &one, 4);
            cheat_pending = 0;
        } else if (++cheat_tries > 60) {
            cheat_pending = 0;
        }
    }
    float SectionX(int i) const {
        const float from = static_cast<float>(i - tab_from), to = static_cast<float>(i - play_tab);
        return (from + (to - from) * Ease(play_tick - tab_at, SectionFrames)) * SectionShift;
    }

    // Tap codes: 0 background, 1 HUD page, 2 cheats page, 3 back, 10.. the HUD's counters, 20 / 21 the cheat's
    // previous / next choice, 22 apply it; the Epilogue's 23 gauge refill, 24 / 25 a thousand magic points less /
    // more. `base`: the tab's section x.
    void OptionsStep(const EdenDsmodHostApi& host, OptionsUi& ui, bool epilogue, u64 tick, bool shown, float base, bool free, bool& boost) {
        const std::string pre = epilogue ? "epo." : "opt.";
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, (pre + name).c_str(), v); };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, (pre + name).c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        const int counters = epilogue ? 2 : 4, first = epilogue ? EpiHud : 1;
        if (const int tap = ui.tap.exchange(-1); tap >= 0 && free && shown) {
            const auto view = [&](int v) { ui.view_from = ui.view, ui.view = v, ui.view_at = tick, boost = !boost; };
            if (ui.view == 0 && tap <= 2) {
                ui.pushed[tap] = tick;
                if (tap == 0)
                    Store(0, options[0] ^ 1);
                else
                    ui.panel = tap, view(tap);
            } else if (ui.view != 0 && tap == 3) {
                ui.pushed[3] = tick, view(0);
            } else if (ui.view == 1 && tap >= 10 && tap < 10 + counters) {
                Store(first + tap - 10, options[static_cast<std::size_t>(first + tap - 10)] ^ 1);
            } else if (ui.view == 2 && tap >= 20 && tap <= (epilogue ? 25 : 22)) {
                ui.pushed[tap - 16] = tick;
                if (epilogue && (tap <= 21 || tap >= 24))
                    epi_points = std::clamp(epi_points + (tap == 20 ? -100 : tap == 21 ? 100 : tap == 24 ? -1000 : 1000), -EpiPointsMax,
                                            EpiPointsMax);
                else if (epilogue && tap == 22)
                    epi_points_pending += epi_points, epi_points = 0, epi_cheat_tries = 0;
                else if (epilogue)
                    epi_fill_pending = true, epi_cheat_tries = 0;
                else if (tap != 22)
                    cheat_index = (cheat_index + (tap == 21 ? 1 : CheatAbilities.size() - 1)) % CheatAbilities.size();
                else
                    cheat_pending = CheatAbilities[cheat_index], cheat_tries = 0;
            }
        }
        // the list leaves to the left as a page comes in from the right
        const float vs = Ease(tick - ui.view_at, SectionFrames);
        const auto at = [&](float from, float to) { return std::lround(base + (from + (to - from) * vs) * SectionShift); };
        pub("lx", at(ui.view_from ? -1.0f : 0.0f, ui.view ? -1.0f : 0.0f));
        // the list sits a screen to the left while a page is open: it would show over the section before
        pub("list_on", ui.view == 0 || tick - ui.view_at < static_cast<u64>(SectionFrames) ? 1 : 0);
        pub("px", at(ui.view_from ? 0.0f : 1.0f, ui.view ? 0.0f : 1.0f));
        pub("is_hud", ui.panel == 1 ? 1 : 0);
        pub("is_cheat", ui.panel == 2 ? 1 : 0);
        // each section's touch areas only while it is the one shown (they would take the others' taps)
        pub("in_list", shown && ui.view == 0 ? 1 : 0);
        pub("in_page", shown && ui.view != 0 ? 1 : 0);
        pub("in_hud", shown && ui.view == 1 ? 1 : 0);
        pub("in_cheat", shown && ui.view == 2 ? 1 : 0);
        pub("can", free && shown ? 1 : 0);
        if (title_dir.empty())
            return;
        const Draw draw = shown ? Draw::Normal : Draw::Ahead;
        const auto swap = [&](Swap& sw, const std::string& key, const std::string& name) {
            SwapStep(host, sw, key, (pre + name).c_str(), (pre + name + "_next").c_str(), tick, draw);
        };
        swap(ui.row0, "optrow/" + title_dir + "/0/" + std::to_string(options[0]), "row.0");
        // the pages' fixed pictures, drawn ahead and handed over once drawn
        const auto ahead = [&](const std::string& name, const std::string& key) {
            FilesAhead(key);
            if (FilesDone(key))
                text(name, key);
        };
        ahead("row.1", "optrow/" + title_dir + "/1/0");
        ahead("row.2", "optrow/" + title_dir + "/2/0");
        ahead("panel.1", "optpanel/" + title_dir + "/1");
        ahead("panel.2", (epilogue ? "epopanel/" : "optpanel/") + title_dir + "/2");
        // optline/<dir>/<line>/<hidden>: the stage's counters 0..3, then the Epilogue's 5, 6
        for (int i = 0; i < counters; ++i)
            swap(ui.lines[i], "optline/" + title_dir + "/" + std::to_string(epilogue ? 5 + i : i) + "/" + std::to_string(options[first + i]),
                 "line." + std::to_string(i));
        if (epilogue) {
            ahead("label.0", "optline/" + title_dir + "/7/0");
            ahead("label.1", "optline/" + title_dir + "/8/0");
            swap(ui.card, "epocard/" + std::to_string(epi_points), "card");
            // the refill needs the gauge learnt, the points an amount
            text("apply.0", "epoapply/" + title_dir + (epi_learnt.cannon ? "/0" : "/1"));
            text("apply.1", "epoapply/" + title_dir + (epi_points ? "/0" : "/1"));
            FilesAhead("epoapply/" + title_dir + "/0");
            FilesAhead("epoapply/" + title_dir + "/1");
            text("back2", "dblarrow/1");
            text("next2", "dblarrow/0");
        } else {
            ahead("label", "optline/" + title_dir + "/4/0");
            swap(ui.card, "optcard/" + title_dir + "/" + std::to_string(CheatAbilities[cheat_index]), "card");
            ahead("apply", "optapply/" + title_dir);
        }
        FilesAhead("optapply/" + title_dir);
        text("back", "ablarrow/1/0");
        text("next", "ablarrow/0/0");
        for (int i = 0; i < static_cast<int>(ui.pushed.size()); ++i) {
            float scale = 1;
            if (const float q = static_cast<float>(tick - ui.pushed[i]); tick >= ui.pushed[i] && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            pub("scale." + std::to_string(i), std::lround(scale * 1000));
        }
    }

    std::shared_ptr<const KirbyModes::AbilityPages> AbilityOf(const std::string& dir, int id) {
        const std::string key = dir + "/" + std::to_string(id);
        {
            std::scoped_lock lock{abl_mutex};
            if (const auto it = abl_content.find(key); it != abl_content.end())
                return it->second;
        }
        auto pages = KirbyModes::AbilityContent(romfs, dir, id);
        if (!pages)
            return nullptr;
        auto shared = std::make_shared<const KirbyModes::AbilityPages>(std::move(*pages));
        std::scoped_lock lock{abl_mutex};
        return abl_content.emplace(key, std::move(shared)).first->second;
    }

    // Magolor's moves for the Magic tab's code (EpiMoves), and the description of a restored Magolor or not
    std::shared_ptr<const KirbyModes::AbilityPages> MagolorOf(const std::string& dir, const std::string& moves, bool restored) {
        const std::string key = "magolor/" + dir + "/" + moves + "/" + (restored ? "1" : "0");
        {
            std::scoped_lock lock{abl_mutex};
            if (const auto it = abl_content.find(key); it != abl_content.end())
                return it->second;
        }
        std::array<int, KirbyModes::MagolorTechniques> levels{};
        for (std::size_t t = 0; t < levels.size() && t < moves.size(); ++t)
            levels[t] = moves[t] - '0';
        auto pages = KirbyModes::MagolorContent(romfs, dir, levels, restored);
        if (!pages)
            return nullptr;
        auto shared = std::make_shared<const KirbyModes::AbilityPages>(std::move(*pages));
        std::scoped_lock lock{abl_mutex};
        return abl_content.emplace(key, std::move(shared)).first->second;
    }

    static float Ease(u64 since, int frames) {
        const float t = std::min(1.0f, static_cast<float>(since) / static_cast<float>(frames));
        return 1 - (1 - t) * (1 - t) * (1 - t);
    }

    void PagerStep(const EdenDsmodHostApi& host, Pager& pg, const std::string& pre, const std::string& content, int pages,
                   const std::function<std::string(int)>& page_key, const std::string& info, u64 tick, bool shown, bool free, long x,
                   bool& boost) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, (pre + "." + name).c_str(), v); };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, (pre + "." + name).c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        for (int p = 0; p < pages; ++p)
            FilesAhead(page_key(p));
        if (content != pg.content && FilesReady(page_key(0)))
            pg.content = content, pg.page = 0, pg.at = 0, pg.key[pg.set] = page_key(0);
        if (const int tap = pg.tap.exchange(-1); tap >= 0 && tap <= 1 && free && shown && pages > 1 && content == pg.content) {
            pg.pushed[tap] = tick;
            pg.dir = tap == 1 ? 1 : -1;
            pg.page = (pg.page + pg.dir + pages) % pages;
            pg.set ^= 1;
            pg.key[pg.set] = page_key(pg.page);
            pg.at = tick, boost = !boost;
        }
        const float ps = pg.at ? Ease(tick - pg.at, PageFrames) : 1;
        for (int s = 0; s < 2; ++s) {
            const std::string slot = std::string{"ab"[s]} + ".";
            if (!pg.key[s].empty())
                text(slot + "page", pg.key[s]);
            const float shift = s == pg.set ? static_cast<float>(pg.dir) * PageShift * (1 - ps)
                                : ps < 1    ? -static_cast<float>(pg.dir) * PageShift * ps
                                            : 4 * PageShift;
            pub(slot + "x", x + std::lround(shift));
        }
        // drawn ahead while the section is not shown
        const Draw draw = shown ? Draw::Normal : Draw::Ahead;
        const std::string point = pre + ".info", pill = pre + ".pill";
        SwapStep(host, pg.info, info, point.c_str(), (point + "_next").c_str(), tick, draw);
        SwapStep(host, pg.pill, pre + "pill/" + std::to_string(pg.page + 1) + "/" + std::to_string(pages), pill.c_str(), (pill + "_next").c_str(),
                 tick, draw);
        const std::string off = pages > 1 ? "0" : "1";
        text("arrow.0", pre + "arrow/1/" + off);
        text("arrow.1", pre + "arrow/0/" + off);
        for (int i = 0; i < 2; ++i) {
            float scale = 1;
            if (const float q = static_cast<float>(tick - pg.pushed[i]); tick >= pg.pushed[i] && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            pub("scale." + std::to_string(i), std::lround(scale * 1000));
        }
        pub("sx", x);
        pub("shown", shown ? 1 : 0);
        pub("can", free && shown && pages > 1 ? 1 : 0);
    }

    // The sections, tabs and Abilities pages as they stand; published on every tick (the runtime drops
    // what is not), between two rooms too.
    void AbilityStep(const EdenDsmodHostApi& host, int id, bool free) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        // section i sits (i - tab) screens to the right; the tab shown is raised
        const float tt = Ease(play_tick - tab_at, TabFrames);
        const auto section = [&](int i) { return std::lround(SectionX(i)); };
        pub("play.sx", section(0));
        const long abl_x = section(1);
        for (int t = 0; t < 3; ++t) {
            const float from = t == tab_from ? 0 : TabDrop, to = t == play_tab ? 0 : TabDrop;
            pub("play.ty." + std::to_string(t), std::lround(from + (to - from) * tt));
        }
        const auto content = AbilityOf(title_dir, id);
        const int pages = content ? std::max<int>(1, static_cast<int>(content->pages.size())) : 1;
        const std::string shown = title_dir + "/" + std::to_string(id);
        PagerStep(host, abl, "abl", shown, pages, [&](int p) { return "ablpage/" + shown + "/" + std::to_string(p); }, "ablinfo/" + shown,
                  play_tick, play_tab == 1, free, abl_x, play_boost);
        if (abl.content == shown)
            abl_id = id;
        pub("play.in_game", play_tab == 0 ? 1 : 0);
        OptionsStep(host, opt, false, play_tick, play_tab == 2, SectionX(2), free, play_boost);
        if (free)
            CheatStep(host);
        pub("play.boost", play_boost ? 1 : 0);
    }
    int amiibo_step{};
    u64 resume_pushed{~u64{0} >> 1};
    bool play_resumable{};
    int play_busy{}; // ticks the game has been in a menu or loading
    int play_single{}; // ticks the stage HUD has shown a single sphere
    bool play_posing{}, play_on{}, play_seen{};
    int play_ship{-1}; // Kirby throws a boss's ship part to the Lor: 1, another state 0, no Kirby -1
    u64 play_state{}; // Kirby's state's vtable as a main offset, 0 without Kirby
    int talk_leave{}; // ticks Kirby has been leaving a talk
    bool ship_cover{};
    bool play_drop{}, play_stage_shown{}; // as last shown, kept while the controls fade out
    bool play_free{}; // Kirby can act: the page is not locked (loading, a menu, a scene of his own)
    std::string play_souv_state; // research (trace): the souvenirs as last read
    u64 amiibo_at{};

    // The top screen's HUD (Step.bflyt, the stage HUD layout): the Options tab hides its counters by
    // clearing their containers' visible bit (pane +0x58 bit 0), which the game's own animations leave
    // alone (they move and fade them). A pane is put back only if this module hid it.
    static constexpr const char* HudPanes[]{"N_SphereBase", "N_SphereBaseLvMap", "N_RestPlayerPanel", "N_StarPanel", "N_StockItemInfo"};
    static constexpr int HudOption[]{1, 1, 2, 3, 4}; // the Options tab setting hiding each pane
    u64 hud_layout{}, hud_scanned{};
    std::array<u32, 5> hud_panes{};
    std::array<bool, 5> hud_hidden{};
    std::string hud_found;

    void HudStep(const EdenDsmodHostApi& host, u64 root) {
        u64 a{}, b{}, layout{}, vt{};
        const auto rd = [&](u64 at, u64& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, 8); };
        if (!host.write_memory || !(rd(root + 0x148, a) && rd(a + 0xB8, b) && rd(b + 0x70, layout) && rd(layout, vt)) ||
            vt != host.main_base + 0x15A18D0)
            return;
        // the layout holds its parts' panes inline (five sphere panels before the counters)
        static constexpr std::size_t Span = 0x40000, NameAt = 0xB0;
        const auto named = [&](u32 pane, std::string_view name) {
            char buf[24]{};
            return pane && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + pane + NameAt, buf, sizeof buf) &&
                   std::string_view{buf, strnlen(buf, sizeof buf)} == name;
        };
        const bool all = std::all_of(hud_panes.begin(), hud_panes.end(), [](u32 p) { return p != 0; });
        if (layout != hud_layout || (!all && play_tick - hud_scanned > 120) || (all && !named(hud_panes[2], HudPanes[2]))) {
            hud_scanned = play_tick;
            std::vector<char> bytes(Span);
            for (std::size_t at = 0; at < Span; at += 0x1000)
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + at, bytes.data() + at, 0x1000))
                    break;
            std::string found = "Kirby DSMod hud panes";
            for (std::size_t i = 0; i < hud_panes.size(); ++i) {
                hud_panes[i] = 0;
                const std::string_view name = HudPanes[i];
                for (std::size_t at = NameAt; at + name.size() + 1 < Span && !hud_panes[i]; at += 8)
                    if (std::string_view{&bytes[at], name.size()} == name && bytes[at + name.size()] == 0)
                        hud_panes[i] = static_cast<u32>(at - NameAt);
                char item[48];
                std::snprintf(item, sizeof item, " %s@%X", HudPanes[i], hud_panes[i]);
                found += item;
            }
            if (layout != hud_layout)
                hud_hidden.fill(false);
            hud_layout = layout;
            if (found != hud_found)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, found.c_str());
            hud_found = found;
        }
        for (std::size_t i = 0; i < hud_panes.size(); ++i) {
            if (!named(hud_panes[i], HudPanes[i]))
                continue;
            u8 flags{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + hud_panes[i] + 0x58, &flags, 1))
                continue;
            const bool hide = options[HudOption[i]] != 0;
            if (hide == hud_hidden[i] && (!hide || !(flags & 1)))
                continue;
            if (hide && (flags & 1))
                flags &= ~1u;
            else if (!hide)
                flags |= 1u;
            hud_hidden[i] = hide;
            host.write_memory(host.userdata, layout + hud_panes[i] + 0x58, &flags, 1);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, (std::string{"Kirby DSMod hud "} + (hide ? "hide " : "show ") + HudPanes[i]).c_str());
        }
    }

    // The device's charge (runtime 21: @device.*; an older runtime or a device without a battery answers the
    // fallback, and the battery is then not shown) and whether it is charging.
    static std::pair<int, bool> BatteryCharge(const EdenDsmodHostApi& host) {
        const auto battery = host.get_i64 ? host.get_i64(host.userdata, "@device.battery_pct", -1) : -1;
        const bool charging = host.get_i64 && host.get_i64(host.userdata, "@device.charging", 0) == 1;
        return {battery >= 0 ? static_cast<int>(std::min<std::int64_t>(battery, 100)) : -1, charging};
    }

    std::string PlayBarKey(const EdenDsmodHostApi& host, int lives, int stars, int spheres, unsigned got, int boss, bool beaten) const {
        const auto [charge, charging] = BatteryCharge(host);
        return "play/" + title_dir + "/" + std::to_string(play_mode) + "/" + std::to_string(lives) + "." + std::to_string(stars) + "/" +
               std::to_string(charge) + (charging ? "c" : "") + "/" + std::to_string(spheres) + "." + std::to_string(got) + "/" +
               std::to_string(boss) + "." + (beaten ? "1" : "0");
    }

    // Dialogues: Magolor's talk window, layout MahoroaWin, has its panes in the stage UI's memory after
    // [[root+0x148]+0xB8], found by N_MahoroaWin's name; each window's RootPane is shown while a dialogue is up
    u64 talk_ui{}, talk_root{}, talk_scanned{};
    bool talk_on{};

    bool Talking(const EdenDsmodHostApi& host, u64 root) {
        const auto rd = [&](u64 at, void* out, std::size_t n) {
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, n);
        };
        static constexpr std::size_t Span = 0x30000, NameAt = 0xB0;
        const auto named = [&](u64 pane, std::string_view name) {
            char buf[24]{};
            return pane && rd(pane + NameAt, buf, sizeof buf) && std::string_view{buf, strnlen(buf, sizeof buf)} == name;
        };
        // the Waddle Dees' (the souvenir shop's) message window, YyCmnMessageWindow: [[root+0x68]+0x468], vtable
        // main+0x15A1920, its layout's RootPane at +0xA18
        u64 x{}, win{}, vt{}, pane{};
        u8 shown{};
        if (rd(root + 0x68, &x, 8) && rd(x + 0x468, &win, 8) && rd(win, &vt, 8) && vt == host.main_base + 0x15A1920 &&
            rd(win + 0xA18, &pane, 8) && named(pane, "RootPane") && rd(pane + 0x58, &shown, 1) && (shown & 1))
            return true;
        u64 a{}, ui{};
        if (!rd(root + 0x148, &a, 8) || !rd(a + 0xB8, &ui, 8) || !ui)
            return false;
        if (ui != talk_ui || !named(talk_root, "RootPane")) {
            if (ui == talk_ui && play_tick - talk_scanned < 120)
                return false;
            talk_ui = ui, talk_root = 0, talk_scanned = play_tick;
            std::vector<char> bytes(Span);
            std::size_t got = 0;
            for (std::size_t n; got < Span; got += n) {
                n = std::min<std::size_t>(Span - got, 0x1000 - ((ui + got) & 0xFFF));
                if (!rd(ui + got, bytes.data() + got, n))
                    break;
            }
            static constexpr std::string_view Window = "N_MahoroaWin";
            for (std::size_t at = NameAt; at + Window.size() + 1 < got; at += 8)
                if (std::string_view{&bytes[at], Window.size()} == Window && bytes[at + Window.size()] == 0) {
                    u64 parent{};
                    if (rd(ui + at - NameAt + 0x18, &parent, 8) && named(parent, "RootPane"))
                        talk_root = parent;
                    break;
                }
        }
        u8 flags{};
        return talk_root && rd(talk_root + 0x58, &flags, 1) && (flags & 1);
    }

    void PlaySpheres(const EdenDsmodHostApi& host, u64 root) {
        u64 a{}, b{}, layout{}, vt{};
        const auto rd = [&](u64 at, u64& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, 8); };
        play_spheres = 0, play_got = 0, play_stage = false;
        play_hud = rd(root + 0x148, a) && rd(a + 0xB8, b) && rd(b + 0x70, layout) && rd(layout, vt) && vt == host.main_base + 0x15A18D0;
        if (!play_hud)
            return;
        static constexpr std::size_t Span = 0x14000, NameAt = 0xB0;
        // the panes found are kept per layout object, checked by name: one read while the game still builds
        // the layout (or a new layout where an old one was) would place them wrong
        const auto named = [&](u32 pane, std::string_view name) {
            char buf[24]{};
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + pane + NameAt, buf, sizeof buf) &&
                   std::string_view{buf, strnlen(buf, sizeof buf)} == name;
        };
        // (looked for again until all five spheres' panes are found: a layout still being built has only some of them)
        if (layout != play_layout || play_panes.size() < 11 || !named(play_panes[0], "N_SpherePlacement") || !named(play_panes[1], "L_Sphere1")) {
            // read up to each page's end: what follows the layout object may be unmapped
            std::vector<char> bytes(Span);
            play_panes.clear();
            for (std::size_t at = 0, n; at < Span; at += n) {
                n = std::min<std::size_t>(Span - at, 0x1000 - ((layout + at) & 0xFFF));
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + at, bytes.data() + at, n))
                    break;
            }
            const auto find = [&](std::string_view name, std::size_t from) -> std::size_t {
                for (std::size_t at = std::max(from, NameAt); at + name.size() + 1 < Span; at += 8)
                    if (std::string_view{&bytes[at], name.size()} == name && bytes[at + name.size()] == 0)
                        return at - NameAt;
                return 0;
            };
            const std::size_t placement = find("N_SpherePlacement", 0);
            if (!placement) {
                play_layout = 0;
                return;
            }
            play_panes.push_back(static_cast<u32>(placement));
            for (int i = 1; i <= 5; ++i) {
                const std::size_t part = find("L_Sphere" + std::to_string(i), placement), on = part ? find("P_SphereON02", part) : 0;
                if (!part || !on)
                    break;
                play_panes.push_back(static_cast<u32>(part));
                play_panes.push_back(static_cast<u32>(on));
            }
            play_layout = layout;
        }
        const auto shown = [&](u32 pane) {
            u8 flags{};
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout + pane + 0x58, &flags, 1) && (flags & 1);
        };
        if (play_panes.empty() || !shown(play_panes[0]))
            return;
        play_stage = true;
        for (std::size_t i = 1; i + 1 < play_panes.size(); i += 2)
            if (shown(play_panes[i])) {
                if (shown(play_panes[i + 1]))
                    play_got |= 1u << play_spheres;
                ++play_spheres;
            }
    }

    void PlayStep(const EdenDsmodHostApi& host, bool on) {
        ++play_tick;
        u64 root{};
        u32 menu[2]{};
        const bool read = dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x16325B8, &root, 8) &&
                          dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, root + 0x378, menu, sizeof menu);
        // the word after it is 0x7FBFFFxx: xx 0xFF while loading, 1 for the few frames the game freezes the
        // action (inhaling, a pick-up, a copy ability's pose), which must not flash the page; only a menu
        // or a load that lasts a few frames locks it
        // (while the game loads, the area holds other data: its high bytes no longer read 0x7FBFFF)
        const bool sane = read && (menu[1] >> 8) == 0x7FBFFF, loading = !sane || (menu[1] & 0xFF) == 0xFF;
        // a talk (Magolor in the Lor, the souvenir shop's Waddle Dee) holds Kirby in StateEnterMahoroa or
        // StateEnterOmenClerk from its start, before its window opens and between its lines and menus, then about a
        // second in the matching StateLeave... (capped: Kirby's state as read last tick)
        const bool enter = play_state == 0x15EF9B8 || play_state == 0x15EFA38;
        const bool leave = play_state == 0x15EFE70 || play_state == 0x15EFEC8;
        talk_leave = leave ? talk_leave + 1 : 0;
        const bool talking = on && sane && (enter || (leave && talk_leave < 60) || Talking(host, root));
        if (talking != talk_on)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, talking ? "Kirby DSMod talk on" : "Kirby DSMod talk off");
        talk_on = talking;
        const bool busy = loading || menu[0] != 0 || play_posing || talking;
        play_busy = busy ? play_busy + 1 : 0;
        // (a talk locks the page at once: it never lasts only a few frames)
        const bool free = on && play_busy < 4 && !talking;
        play_free = free;
        const bool paused = !loading && menu[0] == 1;
        // the amiibo button: the pause menu, then its Y (the amiibo dialog), then its "Yes"
        if (amiibo_step > 0 && play_tick - amiibo_at > 240)
            amiibo_step = 0;
        if (amiibo_step == 1 && paused && play_tick - amiibo_at > 40)
            Press("y"), amiibo_step = 2, amiibo_at = play_tick;
        else if (amiibo_step == 2 && play_tick - amiibo_at > 40)
            Press("a"), amiibo_step = 0;
        if (play_tick % 6 == 0)
            PlaySpheres(host, root);
        if (read)
            HudStep(host, root);
        // the page shows once a stage or a hub is up (its HUD and the running scene), not while the game
        // loads, when the HUD may already exist
        u64 scene{};
        dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x1642EE8, &scene, 8);
        // the copy ability (data file "ability"): Kirby's own ones and the super abilities (1..32) can be
        // dropped, not the other characters
        u64 p1{}, p2{}, comp{};
        u32 ability{};
        dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, root + 0x260, &p1, 8) &&
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p1 + 0x140, &p2, 8) &&
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p2 + 0x220, &comp, 8) &&
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, comp + 0x30, &ability, 4);
        const bool droppable = ability >= 1 && ability <= 32;
        play_ability = ability;
        // the component's +0x48 is 1 while Kirby plays a scene of his own (taking an ability, a super
        // ability's, walking through a door): he cannot be moved then; it locks the page from the next tick
        u8 posing{};
        play_posing = comp && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, comp + 0x48, &posing, 1) && posing == 1;
        // Kirby's record +0x2A4 is 1 while the game holds him for a scene: a boss's intro, his defeat until
        // the stage restarts, a door
        u32 held{};
        play_posing |= p1 && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p1 + 0x2A4, &held, 4) && held == 1;
        // Kirby's state machine [Kirby+0x6C0] builds the running state in place at +0xA0: StateShipRepairDemo and its
        // After are the ship part's throw to the Lor after a boss's first defeat
        u64 machine{}, state{};
        const bool known = p2 && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p2 + 0x6C0, &machine, 8) &&
                           dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, machine + 0xA0, &state, 8) && state;
        play_ship = !known ? -1 : state == host.main_base + 0x15EB588 || state == host.main_base + 0x15EB5E0 ? 1 : 0;
        play_state = known ? state - host.main_base : 0;
        const bool inside = on && files_cursor <= 2 && !title_dir.empty() && host.publish_text;
        // Kirby's own objects are gone from a room's fade-out on: from there until the next room is up
        // the page keeps only its background
        play_loading = loading;
        play_gone = !scene || !comp;
        play_between = !inside || !play_hud || play_gone || loading;
        if (play_between) {
            play_tap = -1;
            play_on = false;
            host.publish_i64(host.userdata, "play.paused", 0);
            host.publish_i64(host.userdata, "play.on", 0);
            // between two rooms the page keeps only its background (its controls are hidden, not locked)
            // the controls fade out on play.vis; the rest keeps its last state meanwhile
            host.publish_i64(host.userdata, "play.vis", 0);
            host.publish_i64(host.userdata, "play.lock", 0);
            host.publish_i64(host.userdata, "play.vis_drop", play_drop ? 1 : 0);
            host.publish_i64(host.userdata, "play.vis_nodrop", play_drop ? 0 : 1);
            host.publish_i64(host.userdata, "play.vis_noamiibo", play_stage_shown ? 0 : 1);
            for (const char* key : {"play.bar", "play.souv"}) {
                const auto& swap = std::string_view{key} == "play.bar" ? play_bar : play_souv;
                if (!swap.shown.empty())
                    host.publish_text(host.userdata, key, ("module:kirby:gen/files/" + swap.shown).c_str());
            }
            for (int t = 0; t < 3; ++t)
                host.publish_text(host.userdata, ("play.tab." + std::to_string(t)).c_str(),
                                  ("module:kirby:gen/files/tab/" + title_dir + "/" + std::to_string(t)).c_str());
            host.publish_text(host.userdata, "play.rbtn.0", ("module:kirby:gen/files/rbtn/" + title_dir + "/pause/0").c_str());
            host.publish_text(host.userdata, "play.rbtn.1", ("module:kirby:gen/files/rbtn/" + title_dir + "/discard/0").c_str());
            host.publish_text(host.userdata, "play.rbtn.2", ("module:kirby:gen/files/rbtn/" + title_dir + "/discard/1").c_str());
            for (int i = 0; i < KirbyModes::PlayButtons; ++i)
                host.publish_i64(host.userdata, ("play.can." + std::to_string(i)).c_str(), 0);
            // the tabs' sections keep their places between rooms; nothing of them is drawn before a game runs
            if (!title_dir.empty() && play_seen)
                AbilityStep(host, std::max(abl_id, 0), false);
            return;
        }
        host.publish_i64(host.userdata, "play.vis", 1);
        play_seen = true;
        // souvenirs and amiibo are for stages: in a hub or the Lor X opens Merry Magoland instead (a stage shows its
        // sphere counter, but Magolor's rooms do not: any room with the HUD inside a world counts)
        const bool in_stage = play_stage || (play_hud && play_world >= 0);
        // a souvenir is taken in a stage, and anywhere in the Arenas
        const bool usable = in_stage || arena_mode;
        // the file's live block: lives u32 +0x26C, stars +0x270 per mode; souvenir counts at the block's
        // +0x2EF0 (5 x u32), the reserve slot at +0x2D30 (0 empty, n = count index + 1)
        int lives{}, stars{}, reserve{};
        bool souvenirs = false;
        // a boss's stage has no spheres (the game's stage table), yet its HUD keeps L_Sphere1, shown by
        // default: one sphere for half a second (not a stage's first frames) means a boss; the bar then shows
        // what beating it gives (its world flag)
        play_single = in_stage && play_spheres == 1 ? play_single + 1 : 0;
        const int boss = play_single >= 30 ? play_world : -1;
        if (boss >= 0)
            play_boss = boss;
        u8 beaten{};
        // the counters the game's HUD shows: [[[main+0x16432A0]+0x650]+0x180], lives at +0x190, point stars
        // at +0x194 (live in stages and hubs; the save block only catches up when the game saves)
        u64 hud{}, owner{}, counters{};
        u32 live[2]{};
        const bool counted = dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x16432A0, &hud, 8) &&
                             dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, hud + 0x650, &owner, 8) &&
                             dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, owner + 0x180, &counters, 8) &&
                             dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, counters + 0x190, live, sizeof live) &&
                             live[0] < 1000 && live[1] < 10000;
        if (const u64 data = SlotData(host, static_cast<int>(files_cursor), true)) {
            u32 v[2]{}, counts[5]{}, slot{};
            const u64 base = data + (play_mode ? 0x6C8 : 0);
            if (counted)
                v[0] = live[0], v[1] = live[1];
            if (boss >= 0)
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, base + 0xF8 + 8 * boss, &beaten, 1);
            if (counted || dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, base + 0x26C, v, sizeof v))
                lives = static_cast<int>(std::min(v[0], 999u)), stars = static_cast<int>(std::min(v[1], 9999u));
            if (dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data - 0x28 + 0x2EF0, counts, sizeof counts) &&
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data - 0x28 + 0x2D30, &slot, 4)) {
                reserve = slot <= 5 ? static_cast<int>(slot) : 0;
                souvenirs = reserve > 0 || std::any_of(std::begin(counts), std::end(counts), [](u32 n) { return n > 0 && n < 1000; });
            }
        }
        // a tap: the button's push, then what it does
        if (const int tap = play_tap.exchange(-1); tap >= 0 && tap < KirbyModes::PlayButtons && free) {
            const bool ok = tap <= 2 && play_tab != 0 ? false
                            : tap == 1                ? droppable
                            : tap == 2                ? souvenirs && usable
                            : tap == 3                ? in_stage
                                                      : true;
            // the tabs switch the section
            if (ok && tap >= 4 && tap - 4 != play_tab)
                tab_from = play_tab, play_tab = tap - 4, tab_at = play_tick, play_boost = !play_boost;
            if (ok) {
                play_pushed[tap] = play_tick;
                if (tap == 0)
                    Press("plus");
                else if (tap == 1)
                    Press("minus");
                else if (tap == 2)
                    use_requested = true;
                else if (tap == 3)
                    Press("plus"), amiibo_step = 1, amiibo_at = play_tick;
            }
        }
        // the device's charge (runtime 21: @device.*; an older runtime or a device without a battery
        // answers the fallback, and the battery is then not shown)
        const int charge = BatteryCharge(host).first;
        const std::string key = PlayBarKey(host, lives, stars, play_spheres == 1 ? 0 : play_spheres, play_got, boss, beaten);
        host.publish_i64(host.userdata, "play.battery", charge >= 0 ? 1 : 0);
        // the bar first: the page's largest picture, which would otherwise land last
        SwapStep(host, play_bar, key, "play.bar", "play.bar_next", play_tick, Draw::Urgent);
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        // the pause menu open: Continue takes the Pause button's place, over the lock, and closes it with Plus
        // (not while the amiibo button drives the pause menu itself)
        const bool resumable = paused && amiibo_step == 0 && on;
        // it comes in with the push its Pause twin would have shown (the pause menu covers it at once)
        if (resumable && !play_resumable)
            resume_pushed = play_tick;
        play_resumable = resumable;
        if (resume_tap.exchange(false) && resumable)
            resume_pushed = play_tick, Press("plus");
        pub("play.paused", resumable ? 1 : 0);
        {
            float scale = 1;
            if (const float q = static_cast<float>(play_tick - resume_pushed); play_tick >= resume_pushed && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            pub("play.scale.resume", std::lround(scale * 1000));
        }
        text("play.rbtn.3", "rbtn/" + title_dir + "/continue/0");
        play_souv_state = "souv=" + std::to_string(souvenirs) + "/" + std::to_string(reserve) + " spheres=" + std::to_string(play_spheres) +
                          " world=" + std::to_string(play_world) + " mode=" + std::to_string(play_mode);
        // (the Arenas take the story's look, whichever mode was played last)
        SwapStep(host, play_souv, "souv/" + std::to_string(reserve) + "/" + std::to_string(arena_mode ? 0 : play_mode) + "/" + (souvenirs && usable ? "0" : "1"),
                 "play.souv", "play.souv_next", play_tick);
        text("play.rbtn.0", "rbtn/" + title_dir + "/pause/0");
        text("play.rbtn.1", "rbtn/" + title_dir + "/discard/0");
        text("play.rbtn.2", "rbtn/" + title_dir + "/discard/1");
        for (int t = 0; t < 3; ++t)
            text("play.tab." + std::to_string(t), "tab/" + title_dir + "/" + std::to_string(t));
        pub("play.lock", free ? 0 : 1);
        play_drop = droppable, play_stage_shown = in_stage;
        pub("play.vis_drop", droppable ? 1 : 0);
        pub("play.vis_nodrop", droppable ? 0 : 1);
        // the game only takes amiibo in a stage (in a hub its pause menu says so)
        pub("play.vis_noamiibo", in_stage ? 0 : 1);
        for (int i = 0; i < KirbyModes::PlayButtons; ++i) {
            // ModeSelectBtn*_Push: down to 0.92 and back in 8 frames
            float scale = 1;
            if (const float q = static_cast<float>(play_tick - play_pushed[i]); play_tick >= play_pushed[i] && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            pub("play.scale." + std::to_string(i), std::lround(scale * 1000));
            const bool can = i <= 2 && play_tab != 0 ? false : i == 1 ? droppable : i == 2 ? souvenirs && usable : i == 3 ? in_stage : true;
            pub("play.can." + std::to_string(i), free && can ? 1 : 0);
        }
        AbilityStep(host, ability <= 38 ? static_cast<int>(ability) : 0, free);
        // (the Arenas have their own page with these controls)
        pub("play.on", arena_mode ? 0 : 1);
        play_on = true;
    }

    // Magolor Epilogue's page. From the file's live save block: the techniques' levels (12 u32 at 0xFA8,
    // Magolor's level by LevelFrom, as on the mode select), Magolor Cannon and Black Hole learnt (their
    // unlock bytes 0xF9C + 6 and + 7) and the magic points (0xF94); the special gauge live (EpilogueHud).
    static constexpr int EpiLevels = 10;
    static constexpr int EpiLevelFrom[EpiLevels]{7, 15, 22, 26, 32, 35, 39, 45, 57, 69};
    u64 epi_tick{}, epi_resume_pushed{};
    std::array<u64, 8> epi_pushed = [] {
        std::array<u64, 8> a;
        a.fill(~u64{0} >> 1);
        return a;
    }();
    bool epi_resumable{}, epi_boost{};
    int epi_tab{}, epi_tab_from{}, epi_gauge{};
    Pager epim; // the Magic tab's pages
    static constexpr int EpiStripCells = 10; // keep in sync with EPIM_CELLS in gen_manifest.py
    struct EpiSave {
        int techniques{}, magic{}, gauge{}; // gauge in degrees round the ring
        bool cannon{}, hole{}, heal{}, blast{};
        std::array<int, KirbyModes::Techniques> levels{}; // msbt order, 0 not learnt
    } epi_save;

    std::optional<EpiSave> EpiSaveOf(const EdenDsmodHostApi& host, int slot) const {
        const u64 data = SlotData(host, slot, true);
        u32 levels[12]{}, magic{}, gauge{};
        u8 learnt[12]{}, area2{}, area3{};
        // the special gauge at 0x12A4, 0..10000 (the HUD's Gauge animation frame): what it refills to after a load
        if (!data || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0xFA8, levels, sizeof levels) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0xF9C, learnt, sizeof learnt) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0xF94, &magic, 4) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0x12A4, &gauge, 4) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0xFE3 + 12, &area2, 1) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0xFE3 + 20, &area3, 1))
            return std::nullopt;
        // Heal comes with the second area's boss beaten (its stage-done flag, as on the mode select), Magolor Blast
        // with Black Hole and the third area's boss beaten: the game sets a boss's flag back in the hub once its own
        // top-screen notices are over, while it grants Black Hole already in the boss's room
        EpiSave e{0, static_cast<int>(std::min(magic, 999999u)),
                  static_cast<int>((std::min(gauge, 10000u) * 360 + 5000) / 10000), learnt[6] != 0, learnt[7] != 0,
                  learnt[6] != 0 && area2 != 0, learnt[6] != 0 && learnt[7] != 0 && area3 != 0};
        for (const u32 l : levels)
            e.techniques += static_cast<int>(std::min(l, 99u));
        // (the save keeps Craftiness before Vitality)
        for (std::size_t t = 0; t < e.levels.size(); ++t) {
            const std::size_t at = t == 10 ? 11 : t == 11 ? 10 : t;
            e.levels[t] = learnt[at] ? static_cast<int>(std::min(levels[at], 9u)) : 0;
        }
        return e;
    }

    static int EpiLevel(const EpiSave& e) {
        int level = 0;
        for (const int from : EpiLevelFrom)
            level += e.techniques >= from;
        return level;
    }
    // the techniques' levels, a digit each (the Magic tab's moves: the first MagolorTechniques)
    static std::string EpiMoves(const EpiSave& e, std::size_t count = KirbyModes::MagolorTechniques) {
        std::string code;
        for (std::size_t t = 0; t < count; ++t)
            code += static_cast<char>('0' + e.levels[t]);
        return code;
    }

    // the bar's key: Magolor's level, the techniques he still needs for the next, the magic points and the charge
    std::string EpiBarKey(const EdenDsmodHostApi& host, const EpiSave& e) const {
        const int level = EpiLevel(e);
        const int missing = level < EpiLevels ? EpiLevelFrom[level] - e.techniques : 0;
        const auto [charge, charging] = BatteryCharge(host);
        return "epibar/" + title_dir + "/" + std::to_string(level) + "/" + std::to_string(missing) + "/" + std::to_string(e.magic) + "/" +
               std::to_string(charge) + (charging ? "c" : "");
    }

    // the page's pictures for the chosen file, drawn ahead on the menus (soon: its menu button is under the cursor)
    // and handed to the host once drawn (epipre.<n>, invisible widgets on the main menu's and the Epilogue's
    // pages), so the page shows whole: the host loads a picture only when a widget shows it
    static constexpr int EpiPreload = 90; // keep in sync with EPI_PRELOAD in gen_manifest.py
    void EpiPrewarm(const EdenDsmodHostApi& host, int slot, bool soon) {
        std::vector<std::string> keys;
        const auto want = [&](const std::string& key) {
            if (soon)
                FilesReady(key);
            else
                FilesAhead(key);
            keys.push_back(key);
        };
        const auto save = EpiSaveOf(host, slot);
        if (!save)
            return;
        for (const char* part : {"charging", "full", "particles1", "particles2", "glow", "edge", "front", "locked"})
            keys.push_back(std::string{"epigauge/"} + part);
        for (int k = 0; k < KirbyModes::GaugeSegments; ++k)
            keys.push_back("epigauge/empty/" + std::to_string(k));
        want(EpiBarKey(host, *save));
        want("epirbtn/" + title_dir + "/pause");
        for (int t = 0; t < 3; ++t)
            want("epitab/" + title_dir + "/" + std::to_string(t));
        for (int which = 0; which < 4; ++which)
            for (const int state : (which == 0 ? save->cannon : which == 1 ? save->hole : which == 2 ? save->heal : save->blast)
                                       ? std::initializer_list<int>{1, 2}
                                       : std::initializer_list<int>{0})
                want("episp/" + title_dir + "/" + std::to_string(which) + "/" + std::to_string(state));
        want("epirbtn/" + title_dir + "/continue");
        // every popup the file has still to show, learnt or not, so it shows whole when its time comes
        for (int pop = 1; pop <= 2; ++pop)
            if (!PopupSeenBy(pop, slot))
                want(PopupKey(pop)), want("epipopbtn/" + title_dir);
        for (std::size_t n = 0; n < keys.size() && n < EpiPreload; ++n)
            if (FilesDone(keys[n]))
                host.publish_text(host.userdata, ("epipre." + std::to_string(n)).c_str(), ("module:kirby:gen/files/" + keys[n]).c_str());
    }
    u64 epi_tab_at{}, epi_full_at{};
    Swap epi_bar;
    u64 epi_hud_at{};
    int epi_shown{-1}; // the HUD gauge's N_SRT alpha, -1 not found

    std::string epi_state;
    int epi_free{}, epi_seen{-1}, epi_busy{};
    float epi_smooth{};
    int epi_level{};
    u64 epi_boost_at{};
    bool epi_started{}, epi_settled{}, epi_live{}, epi_save_ok{};
    struct EpiLearnt {
        bool cannon{}, hole{}, heal{}, blast{};
        bool operator==(const EpiLearnt&) const = default;
    } epi_learnt;
    bool epi_learnt_ok{};
    int epi_learnt_wait{};
    // a magic's popup (1 Heal, 2 Magolor Blast; 0 none): shown, and when its button was tapped (it goes once its push
    // is over)
    int epi_pop{}, epi_pop_shown{};
    bool epi_hub{};
    std::string epi_held; // the scene animation holding Magolor, "" when free (logged on change)
    u64 epi_pop_close{}, epi_pop_pushed{~u64{0} >> 1};
    bool PopupSeenBy(int pop, int slot) const {
        return !options_known || options[static_cast<std::size_t>((pop == 1 ? HealSeen : BlastSeen) + slot)] != 0;
    }
    int PopupDue(const EpiSave& e, int slot) const {
        return e.heal && !PopupSeenBy(1, slot) ? 1 : e.blast && !PopupSeenBy(2, slot) ? 2 : 0;
    }
    std::string PopupKey(int pop) const {
        return "epipop/" + title_dir + "/" + std::to_string(pop - 1);
    }
    // in the hub, not in a stage (whose own magic points HUD hangs at X+0x1D370, X as for the cheats): a magic learnt
    // in a boss's room shows its popup once Magolor is back in the hub, wherever the game grants it
    // The game holding Magolor for a scene of its own, which his ability component's pose flag (the stage page's
    // play_posing, a door) does not tell: the animation he plays then, by its name in his model's animation table
    // ([model+0x1328], 0x44 a kind, the name first): a stage's entrance and clear, the goal door's poses and dance, the
    // return portal, the altar's scene, a fruit piece's or an item's pick-up. Its name, or "" while he is free.
    std::string MagolorScene(const EdenDsmodHostApi& host, u64 root) const {
        static constexpr std::string_view Scenes[]{"Advent", "WakeUp", "Warp", "StageClear", "GoalDoor", "Dance", "Demo", "Hidden",
                                                   "ItemGet", "RareItemGet", "GemGet", "HalfTone"};
        u64 rec{}, hero{}, anim{}, model{}, vt{}, table{};
        u32 kind{};
        char name[24]{};
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (!rd(root + 0x260, rec) || !rd(rec + 0x140, hero) || !rd(hero + 0x130, anim) || !rd(anim + 0x18, model) || !model ||
            !rd(model, vt) || vt != host.main_base + HeroContextVtable || !rd(model + 0x12D8 + 0x38, kind) || kind > 2000 ||
            !rd(model + 0x1328, table) || !table || !rd(table + u64{kind} * 0x44, name))
            return {};
        const std::string_view n{name, strnlen(name, sizeof name)};
        for (const auto scene : Scenes)
            if (n.starts_with(scene))
                return std::string{n};
        return {};
    }
    bool EpiInHub(const EdenDsmodHostApi& host, u64 root) const {
        u64 a{}, mode{}, gauge{}, owner{}, x{}, stage{}, hub{};
        const auto rd = [&](u64 at, u64& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, 8); };
        return rd(root + 0x328, a) && rd(a + 8, mode) && rd(mode + 0x660, gauge) && rd(gauge, owner) && rd(owner + 0xE8, x) &&
               rd(x + 0x1D370, stage) && !stage && rd(x + 0x213B0, hub) && hub;
    }
    void PopupSeen() {
        const auto seen = static_cast<std::size_t>(epi_pop == 1 ? HealSeen : BlastSeen) + std::min(files_cursor, 2u);
        epi_pop = 0, epi_pop_close = 0;
        if (!options[seen])
            Store(static_cast<int>(seen), 1);
    }
    u64 epi_loaded{}, epi_seen_at{};
    static std::string KirbyHex(u64 v) {
        char b[24];
        std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(v));
        return b;
    }

    // The special gauge: L_SpecialSkill's S_scissor_00 cuts it, 194 wide when full. Its layout hangs off the
    // root object at [[[root+0x148]+0xA0]+0x7E0] in a stage (a layout of its own) and inline in the hub's HUD
    // at [[[root+0x1B0]+0x760]+0x98] (the game's layout vtable); found by name, cached per layout.
    // The Epilogue's cheats, written as the game's own code does it (validated 2026-10-21 under the debugger). The
    // gauge G = [[[root+0x328]+8]+0x660]: u32 value +0x10, max +0x14, nonzero +0x20 while a special holds it; the
    // stage's magic points P = [[[root+0x328]+8]+0x668]: cap +0x28, value +0x2C (added to the save's total, 0xF94,
    // when the stage is cleared). The HUD from X = [[G]+0xE8]: a stage's [X+0x1D370] (its points' counter at
    // +0x15E0, its gauge at +0x1610), else the hub's [X+0x213B0] (the total's counter +0x1CA0, the gauge +0x1CD0).
    static constexpr int EpiPointsMax = 99900, EpiTotalMax = 999999;
    static constexpr float EpiCheatRules[]{244, 404}; // the lines under the cheats, on the panel (EPO_* in gen_manifest.py)
    int epi_points{}, epi_points_pending{}, epi_cheat_tries{};
    bool epi_fill_pending{};

    // A HUD counter as the game sets it to a new value (main+0x189AFAC): it counts from the number shown,
    // in `rate` frames a point up to its longest (counter - 0x50); +0 the value, +0x18 the number shown.
    void EpiCounter(const EdenDsmodHostApi& host, u64 c, u32 value) {
        u32 base{}, rate{};
        float shown{}, longest{};
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const auto put = [&](u64 at, auto v) { host.write_memory(host.userdata, at, &v, sizeof v); };
        if (!rd(c, base) || !rd(c + 4, rate) || !rd(c + 0x18, shown) || !rd(c - 0x50, longest) || rate > 60)
            return;
        put(c, value);
        put(c + 0x24, 0u);
        put(c + 0x1C, shown);
        put(c + 0x20, static_cast<float>(value) - shown);
        put(c + 0x28, std::min(std::abs(static_cast<float>((static_cast<s64>(value) - base) * rate)), longest));
        put(c + 8, rate);
    }

    // The HUD's gauge as the game fills it (main+0x8185474C and its caller): the rate, its bar animation's frame,
    // and the request that shows it for a while.
    void EpiGaugeHud(const EdenDsmodHostApi& host, u64 g, float rate) {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const auto put = [&](u64 at, auto v) { host.write_memory(host.userdata, at, &v, sizeof v); };
        put(g + 0x70, rate);
        u64 list{}, key{};
        s32 count{};
        if (rd(g + 0x60, list) && list && rd(g + 0x68, key) && rd(list + 0x320, count) && count > 0 && count <= 12)
            for (s32 i = 0; i < count; ++i) {
                u64 e{}, used{}, owner{}, anim{}, res{};
                u16 frames{};
                if (rd(list + 0x328 + 8 * i, e) && rd(e + 0x60, used) && used && rd(e + 0x68, owner) && owner == key && rd(e + 0x28, anim) &&
                    anim && rd(anim + 0x18, res) && rd(res + 8, frames)) {
                    put(anim + 0x20, rate * frames);
                    break;
                }
            }
        u32 state[6]{}; // +0x7C .. +0x90
        if (rd(g + 0x7C, state) && state[2] != 3) {
            put(g + 0x7C, 0u);
            if (state[1] >= 2 && static_cast<s32>(state[5]) <= 0)
                put(g + 0x88, 1u);
        }
    }

    void EpiCheatStep(const EdenDsmodHostApi& host, u64 root, bool ready) {
        if ((!epi_fill_pending && !epi_points_pending) || !host.write_memory)
            return;
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const auto put = [&](u64 at, auto v) { host.write_memory(host.userdata, at, &v, sizeof v); };
        u64 a{}, mode{}, gauge{}, owner{}, x{}, stage{}, hub{};
        u32 g[2]{}, held{};
        const bool found = ready && rd(root + 0x328, a) && rd(a + 8, mode) && rd(mode + 0x660, gauge) && rd(gauge, owner) &&
                           rd(owner + 0xE8, x) && rd(gauge + 0x10, g) && rd(gauge + 0x20, held) && g[1] > 0 && g[1] <= 100000 && g[0] <= g[1] &&
                           ((rd(x + 0x1D370, stage) && stage) || (rd(x + 0x213B0, hub) && hub));
        if (!found || (epi_fill_pending && held)) {
            // a few seconds at most (a load, a special running)
            if (++epi_cheat_tries > 300)
                epi_fill_pending = false, epi_points_pending = 0;
            return;
        }
        const u64 data = SlotData(host, static_cast<int>(std::min(files_cursor, 2u)), true);
        if (epi_fill_pending) {
            put(gauge + 0x10, g[1]);
            if (data)
                put(data + 0x12A4, g[1]);
            EpiGaugeHud(host, stage ? stage + 0x1610 : hub + 0x1CD0, 1.0f);
            epi_fill_pending = false;
        }
        if (epi_points_pending) {
            // in a stage its own points, in the hub the total; never below 0
            u64 points{};
            u32 p[2]{}, total{};
            if (stage && rd(mode + 0x668, points) && rd(points + 0x28, p) && p[1] <= p[0]) {
                const u32 v = static_cast<u32>(std::clamp<s64>(s64{p[1]} + epi_points_pending, 0, p[0]));
                put(points + 0x2C, v);
                EpiCounter(host, stage + 0x15E0, v);
            } else if (!stage && data && rd(data + 0xF94, total)) {
                const u32 v = static_cast<u32>(std::clamp<s64>(s64{total} + epi_points_pending, 0, EpiTotalMax));
                put(data + 0xF94, v);
                EpiCounter(host, hub + 0x1CA0, v);
            }
            epi_points_pending = 0;
        }
    }

    // The HUD's magic points (a stage's L_MaryokuPanel, the hub's N_Maryoku) and gauge (L_SpecialSkill), hidden
    // by the Options tab as on the stage page: the pane's visible bit, put back only if this module hid it
    // (per HUD layout: a stage's and the hub's both resolve in the hub, where only the second has the gauge)
    struct EpiHudLayout {
        u64 at{}, scissor{}, srt{};
        std::array<u64, 2> panes{};
        std::array<bool, 2> hidden{};
    };
    std::array<EpiHudLayout, 2> epi_huds;

    void EpilogueHud(const EdenDsmodHostApi& host, u64 root) {
        static constexpr std::array<std::array<u32, 3>, 2> Chains{{{0x148, 0xA0, 0x7E0}, {0x1B0, 0x760, 0x98}}};
        for (std::size_t c = 0; c < Chains.size(); ++c) {
            const auto& chain = Chains[c];
            auto& hud = epi_huds[c];
            u64 at = root, vt{};
            bool ok = true;
            for (const u32 off : chain)
                ok = ok && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at + off, &at, 8) && at;
            if (!ok || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &vt, 8) || vt != host.main_base + 0x15A18D0)
                continue;
            if (at != hud.at) {
                hud = {at};
                std::vector<char> m(0xC000);
                if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, m.data(), m.size()))
                    continue;
                // L_SpecialSkill's N_SRT (shown with the gauge, alpha at +0x59), then S_scissor_00
                bool skill = false;
                for (std::size_t o = 0xB0; o + 16 < m.size(); o += 8) {
                    const std::string_view name{&m[o], strnlen(&m[o], 16)};
                    const u64 pane = at + o - 0xB0;
                    if (name == "L_SpecialSkill" && !skill)
                        skill = true, hud.panes[1] = pane;
                    else if ((name == "L_MaryokuPanel" || name == "N_Maryoku") && !hud.panes[0])
                        hud.panes[0] = pane;
                    else if (name == "N_SRT" && skill && !hud.srt)
                        hud.srt = pane;
                    else if (name == "S_scissor_00" && !hud.scissor)
                        hud.scissor = pane;
                }
            }
            for (std::size_t i = 0; i < hud.panes.size() && host.write_memory; ++i) {
                u8 flags{};
                if (!hud.panes[i] || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, hud.panes[i] + 0x58, &flags, 1))
                    continue;
                const bool hide = options[EpiHud + i] != 0;
                if (hide == hud.hidden[i] && (!hide || !(flags & 1)))
                    continue;
                flags = hide ? flags & ~1u : flags | 1u;
                hud.hidden[i] = hide;
                host.write_memory(host.userdata, hud.panes[i] + 0x58, &flags, 1);
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                         ("Kirby DSMod epi hud " + std::string{hide ? "hide " : "show "} + (i ? "gauge" : "points") + " layout " + std::to_string(c)).c_str());
            }
            u8 alpha{};
            epi_shown = hud.srt && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, hud.srt + 0x59, &alpha, 1) ? alpha : -1;
            float width{};
            if (hud.scissor && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, hud.scissor + 0x50, &width, 4) && width >= 0 &&
                width <= 200) {
                epi_gauge = std::clamp(static_cast<int>(std::lround(width / 194 * 360)), 0, 360);
                epi_hud_at = epi_tick;
                return;
            }
        }
    }

    // Magolor Epilogue's Heal: the Cannon's activation (the game's own freeze, charge effects and sound) turned,
    // by patches to the Basil step scripts in RAM, into the Black Hole's opening with no beam and no move, then a
    // short clear dance (A1, A2, the wink pose without its spins, half a second still) to the Festival's music, the
    // place's music back, and an Energy Drink (half the health). basil/ScnStep.bin is loaded verbatim at a page
    // start: it is found by its header, every word is checked before it changes and everything is put back once
    // the dance is over.
    struct StepWord {
        u32 at, from, to; // offset in ScnStep.bin, the game's word, ours
    };
    static constexpr StepWord HealPatches[]{
        {0x2bfe38, 0x0000027c, 0x00000279},
        {0x2bfe88, 0x0000027d, 0x00000029},
        {0x2c0d3c, 0x00760a12, 0x00760ab6},
        {0x2c0d54, 0x00780912, 0x007809b6},
        {0x2c0d5c, 0x00790810, 0x007908b6},
        {0x2c0da8, 0x000e0919, 0x000e09b6},
        {0x2c0db0, 0x007c0810, 0x007c08b6},
        {0x2c0e0c, 0x00180919, 0x001809b6},
        {0x2c0fb0, 0x004b0912, 0x004b09b6},
        {0x26bed8, 0x0000070d, 0xffff06c1},
        {0x26bedc, 0x00110712, 0x0034070b},
        {0x26bee0, 0xffff06c1, 0xffff07c1},
        {0x26bee8, 0x006c070d, 0x00130710},
        {0x26beec, 0x00110712, 0x001107b6},
        {0x26bef0, 0x00130710, 0x001307b6},
        {0x26baac, 0x0100003e, 0x01000040},
        {0x26bc70, 0x000a040f, 0x0020000b},
        {0x26bc74, 0xff040001, 0x0028010b},
        {0x26bc78, 0xff040101, 0x004c020b},
        {0x26bc7c, 0x0003ff18, 0x0030030b},
        {0x26bc80, 0x000b040f, 0x000b04b6},
        {0x26bc84, 0xff040101, 0xff0401b6},
        {0x26bc88, 0xff00011b, 0xff0001b6},
        {0x26bc8c, 0xfffdffbc, 0xfffdffb6},
        {0x26bc90, 0xff000201, 0xff0002b6},
        {0x26bc94, 0x0003ff18, 0x0003ffb6},
        {0x26bc98, 0x000a040f, 0x000a04b6},
        {0x26bc9c, 0xff040201, 0xff0402b6},
        {0x26bca0, 0xff00021b, 0xff0002b6},
        {0x26bca4, 0xfffdffbc, 0xfffdffb6},
        {0x26bca8, 0xff01021b, 0xff0102b6},
        {0x26bcac, 0xfffbffbc, 0xfffbffb6},
        {0x26bcb0, 0x000d040f, 0x000d04b6},
        {0x26bcb4, 0xff040301, 0xff0403b6},
        {0x26bf2c, 0x0000070d, 0x0064070d},
        {0x26bf34, 0xffff06c1, 0x001c070b},
        {0x26bf38, 0x006c070d, 0xffff07c1},
        {0x26bf3c, 0x00110712, 0x006c070d},
        {0x26bf40, 0x00130710, 0x00110712},
        {0x26bf44, 0xff030701, 0x00130710},
        {0x26bf48, 0x0068080b, 0x0050060b},
        {0x26bf4c, 0x0064090d, 0xffff06c1},
        {0x26bf50, 0x00100712, 0x0024070b},
        {0x26bf54, 0x0000070d, 0x00170712},
        {0x26bf58, 0x00110712, 0x001107b6},
        {0x26bf5c, 0xffff06c1, 0xffff06b6},
        {0x26bf60, 0x006c050d, 0x006c05b6},
        {0x26bf64, 0x00110512, 0x001105b6},
        {0x26bf68, 0x0070050b, 0x007005b6},
        {0x26bf6c, 0x00140510, 0x001405b6},
        {0x26bf70, 0x00150510, 0x001505b6},
        {0x26bf74, 0x00130510, 0x001305b6},
        {0x26bfb8, 0x0068020b, 0x000c020b}, // the music lets go of its slot when it ends, should the game cut the dance short
    };
    static constexpr u8 StepHeader[32]{0x58, 0x42, 0x49, 0x4e, 0x34, 0x12, 0x02, 0x00, 0xc2, 0xcb, 0x48, 0x00, 0xe9, 0xfd, 0x00, 0x00,
                                       0x07, 0x00, 0x08, 0x00, 0xa6, 0x02, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x51, 0x11, 0x00, 0x00};
    u64 step_file{};
    int heal_phase{}; // 0 idle, 1 patched and X pressed, 2 the dance running
    u64 heal_at{}, heal_still{};
    bool heal_given{};

    bool StepFileValid(const EdenDsmodHostApi& host, u64 at) const {
        const u8* p = at ? host.get_read_pointer(host.userdata, at, sizeof StepHeader) : nullptr;
        return p && std::memcmp(p, StepHeader, sizeof StepHeader) == 0;
    }

    bool FindStepFile(const EdenDsmodHostApi& host) {
        if (StepFileValid(host, step_file))
            return true;
        step_file = 0;
        const u64 begin = host.get_heap_begin(host.userdata), end = host.get_heap_end(host.userdata);
        for (u64 page = begin & ~u64{0xFFF}; page < end; page += 0x1000)
            if (StepFileValid(host, page))
                return step_file = page, true;
        return false;
    }

    // all or nothing: every word must read as expected before any is written
    bool BasilPatch(const EdenDsmodHostApi& host, u64 file, std::span<const StepWord> words, bool on) {
        if (!host.write_memory || !file)
            return false;
        for (const auto& p : words) {
            u32 v{};
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, file + p.at, &v, 4) || (v != p.from && v != p.to))
                return false;
        }
        for (const auto& p : words)
            host.write_memory(host.userdata, file + p.at, on ? &p.to : &p.from, 4);
        return true;
    }
    bool StepPatch(const EdenDsmodHostApi& host, std::span<const StepWord> words, bool on) {
        return FindStepFile(host) && BasilPatch(host, step_file, words, on);
    }
    bool HealPatch(const EdenDsmodHostApi& host, bool on) {
        return StepPatch(host, HealPatches, on);
    }

    // a message to Magolor's queue (as the ability cheat does), when it is empty and he is free
    bool SendHeroMessage(const EdenDsmodHostApi& host, const std::array<u32, 5>& message) {
        u64 root{}, rec{}, p{}, q{};
        u32 count = 1;
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (!host.write_memory || !rd(host.main_base + 0x16325B8, root) || !rd(root + 0x260, rec) || !rd(rec + 0x140, p) ||
            !rd(p + 0x260, q) || !rd(q + 0x10, count) || count != 0)
            return false;
        const u32 one = 1;
        host.write_memory(host.userdata, q + 0x14, message.data(), sizeof message);
        host.write_memory(host.userdata, q + 0x10, &one, 4);
        return true;
    }

    // From the moment the game takes hold of Magolor: the Black Hole's opening (about 49 frames), A1 at once
    // (the music 23 frames in, its last hit 148 frames later), a 17-frame hold, A2 (67 frames each), then the wink
    // pose's two spins skipped at 17x in 3 frames and its 65th frame, where the original dance says its "kime":
    // the Energy Drink then. Free again for half a second: it is over.
    static constexpr u64 HealAt = 217;
    void HealStep(const EdenDsmodHostApi& host) {
        const auto log = [&](const std::string& m) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod heal " + m).c_str());
        };
        if (heal_phase == 1) {
            if (play_posing)
                heal_phase = 2, heal_at = heal_still = epi_tick, heal_given = false, log("started");
            else if (epi_tick - heal_at > 180)
                heal_phase = 0, log(HealPatch(host, false) ? "not started, restored" : "not started, RESTORE FAILED");
        } else if (heal_phase == 2) {
            if (!heal_given && epi_tick - heal_at >= HealAt && SendHeroMessage(host, {3, 0x23, 0, 0, 0}))
                heal_given = true, log("healed");
            heal_still = play_posing ? epi_tick : heal_still;
            if ((heal_given && epi_tick - heal_still > 30) || epi_tick - heal_at > 1500) {
                heal_phase = 0;
                log(HealPatch(host, false) ? "done, restored" : "done, RESTORE FAILED");
            }
        }
    }

    // Magolor Blast: Festival's hit as a magic. The Cannon's activation (the game's freeze and sound) plays, by patches
    // to the step scripts, the festival dance of the Magolor who joins Kirby's Festival (FestivalStart, his acts with
    // the tambourine, the basket and the bugle, FestivalFinish as the music ends) to Festival's music and confetti,
    // and on the closing pose creates, in place of the beam, the burst of Magolor's dash attack over the screen from
    // the camera's centre with Festival's damage (a Magolor hit all the same: magic points, the combo, the blocks);
    // the gauge is spent there.
    static constexpr StepWord BlastPatches[]{
        {0x2bfe38, 0x0000027c, 0x00000029},
        {0x2bfe5c, 0x01000181, 0x000000e6},
        {0x2bfea8, 0x000000ad, 0x000000ac},
        {0x2bfe48, 0x000003e7, 0x00000193},
        {0x2bfe4c, 0x00000022, 0x00000000},
        {0x2c0458, 0x00120d11, 0x00380d0b},
        {0x2c0bec, 0x00600919, 0x006009b6},
        {0x2c0d3c, 0x00760a12, 0x00760ab6},
        {0x2c0d54, 0x00780912, 0x007809b6},
        {0x2c0d5c, 0x00790810, 0x007908b6},
        {0x2c0db0, 0x007c0810, 0x00790810},
        {0x2c0dd0, 0x001e0a12, 0x001e0ab6},
        {0x2c0e0c, 0x00180919, 0x001809b6},
        {0x2c0fb0, 0x004b0912, 0x004b09b6},
        {0x2c0db4, 0xff000993, 0xff000893},
        {0x2c0db8, 0x0005ffb9, 0x0004ffb9},
        {0x2c0dbc, 0x001c0811, 0xff080a95},
        {0x2c0dc0, 0xff080a01, 0x000dffb9},
        {0x2c0dc4, 0x001d0911, 0x002c0b0b},
        {0x2c0dc8, 0xff090a01, 0x000e0a12},
        {0x2c0dcc, 0x00640b0b, 0x00640bb6},
        {0x2c0df4, 0xff000993, 0xff000893},
        {0x2c0df8, 0x0005ffb9, 0x0004ffb9},
        {0x2c0dfc, 0x006d0811, 0xff080c95},
        {0x2c0e00, 0xff080a01, 0x000dffb9},
        {0x2c0e04, 0x00180b0b, 0x007a0c12},
        {0x2c0e08, 0x006e0911, 0x007b0b11},
        {0x2c0e48, 0xff000893, 0x00030b19},
        {0x2c0e4c, 0x0003ffb9, 0xff000801},
        {0x2c0e50, 0xff080995, 0x007c0810},
        {0x2c0e54, 0x000dffb9, 0x000dffb6},
        {0x2c0e58, 0xff090a01, 0xff090ab6},
        {0x2c0e5c, 0x006c0b0b, 0x006c0bb6},
        {0x2c0e60, 0x000e0a12, 0x000e0ab6},
        {0x2c1398, 0xff010301, 0x00590112},
        {0x2c139c, 0x00970211, 0x009702b6},
        {0x2c13a0, 0xff020301, 0xff0203b6},
        {0x2c13a4, 0x0018040b, 0x001804b6},
        {0x2c13a8, 0x00980312, 0x009803b6},
        {0x2c1260, 0xff001093, 0xff001193},
        {0x2c1268, 0x00450f11, 0x000f1011},
        {0x2c126c, 0x0f051002, 0xff101201},
        {0x2c1270, 0x00461011, 0x003f1111},
        {0x2c1274, 0xff050307, 0x11061202},
        {0x2c1278, 0x0008ffb9, 0x00401211},
        {0x2c127c, 0xff030f01, 0xff030fb6},
        {0x2c1280, 0x0f061002, 0x0f0610b6},
        {0x2c1284, 0x008f1011, 0x008f10b6},
        {0x3be0c0, 0x401ccccd, 0x41800000},
        {0x26baac, 0x0100003e, 0x01000000},
        {0x26bfb8, 0x0068020b, 0x000c020b}, // the music lets go of its slot when it ends, should the game cut the dance short
        {0x26ba78, 0x00000040, 0x0000002a},
        {0x26ba80, 0x00000010, 0x00000024},
        {0x26bc70, 0x000a040f, 0x0020000b},
        {0x26bc74, 0xff040001, 0x0028010b},
        {0x26bc78, 0xff040101, 0x004c020b},
        {0x26bc7c, 0x0003ff18, 0x0030030b},
        {0x26bc80, 0x000b040f, 0x000b04b6},
        {0x26bc84, 0xff040101, 0xff0401b6},
        {0x26bc88, 0xff00011b, 0xff0001b6},
        {0x26bc8c, 0xfffdffbc, 0xfffdffb6},
        {0x26bc90, 0xff000201, 0xff0002b6},
        {0x26bc94, 0x0003ff18, 0x0003ffb6},
        {0x26bc98, 0x000a040f, 0x000a04b6},
        {0x26bc9c, 0xff040201, 0xff0402b6},
        {0x26bca0, 0xff00021b, 0xff0002b6},
        {0x26bca4, 0xfffdffbc, 0xfffdffb6},
        {0x26bca8, 0xff01021b, 0xff0102b6},
        {0x26bcac, 0xfffbffbc, 0xfffbffb6},
        {0x26bcb0, 0x000d040f, 0x000d04b6},
        {0x26bcb4, 0xff040301, 0xff0403b6},
        {0x26bebc, 0x0064040d, 0x00120710},
        {0x26bec0, 0x0064050d, 0x0048060b},
        {0x26bec4, 0xff050678, 0xffff06c1},
        {0x26bec8, 0xff000701, 0x0020070b},
        {0x26bed8, 0x0000070d, 0x00130710},
        {0x26bedc, 0x00110712, 0x0028070b},
        {0x26bee0, 0xffff06c1, 0x0068080b},
        {0x26bee4, 0x00120710, 0x0064090d},
        {0x26bee8, 0x006c070d, 0x00100712},
        {0x26beec, 0x00110712, 0x0040060b},
        {0x26bef0, 0x00130710, 0xffff06c1},
        {0x26bef4, 0xff010701, 0x0030070b},
        {0x26bf04, 0x0000070d, 0x0040060b},
        {0x26bf08, 0x00110712, 0xffff06c1},
        {0x26bf0c, 0xffff06c1, 0x004c070b},
        {0x26bf10, 0x006c070d, 0x0068080b},
        {0x26bf14, 0x00110712, 0x0064090d},
        {0x26bf18, 0x00130710, 0x00100712},
        {0x26bf1c, 0xff020701, 0x0040060b},
        {0x26bf20, 0x0068080b, 0xffff06c1},
        {0x26bf24, 0x0064090d, 0x0038070b},
        {0x26bf28, 0x00100712, 0x0068080b},
        {0x26bf2c, 0x0000070d, 0x0068090d},
        {0x26bf30, 0x00110712, 0x00100712},
        {0x26bf34, 0xffff06c1, 0x00130710},
        {0x26bf38, 0x006c070d, 0x0054060b},
        {0x26bf3c, 0x00110712, 0xffff06c1},
        {0x26bf40, 0x00130710, 0x0024070b},
        {0x26bf44, 0xff030701, 0x00170712},
        {0x26bf48, 0x0068080b, 0x006808b6},
        {0x26bf4c, 0x0064090d, 0x006409b6},
        {0x26bf50, 0x00100712, 0x001007b6},
        {0x26bf54, 0x0000070d, 0x000007b6},
        {0x26bf58, 0x00110712, 0x001107b6},
        {0x26bf5c, 0xffff06c1, 0xffff06b6},
        {0x26bf60, 0x006c050d, 0x006c05b6},
        {0x26bf64, 0x00110512, 0x001105b6},
        {0x26bf68, 0x0070050b, 0x007005b6},
        {0x26bf6c, 0x00140510, 0x001405b6},
        {0x26bf70, 0x00150510, 0x001505b6},
        {0x26bf74, 0x00130510, 0x001305b6}
    };
    // Magolor's animations (C++): Hero.model() is [[hero+0x130]+0x18], Model.anim() is model+0x12D8, its entries
    // [anim+0x50] ([anim+0x48] of them) are {char name[64]; int slot} by Hero.AnimKind, and Anim.Start looks the
    // animation up by that name (with its bone visibility of the same name): the dance's kinds take the festival's.
    // The tambourine's act comes first (its visibility shows the tambourine and hides nothing, the others hide the
    // rest), then basket and bugle
    struct KindName {
        u32 kind;
        std::string_view game, ours;
    };
    static constexpr KindName BlastNames[]{{0x67, "DanceA1", "FestivalStart"},
                                           {0x68, "DanceA2", "FestivalAct03"},
                                           {0x69, "DanceA3", "FestivalAct01"},
                                           {0x6A, "DanceAE", "FestivalAct02"},
                                           {0x6B, "DanceT1", "FestivalFinish"}};
    bool HeroAnimNames(const EdenDsmodHostApi& host, std::span<const KindName> names, bool on) {
        u64 root{}, rec{}, hero{}, holder{}, model{}, table{};
        u32 count{};
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (!host.write_memory || !rd(host.main_base + 0x16325B8, root) || !rd(root + 0x260, rec) || !rd(rec + 0x140, hero) ||
            !rd(hero + 0x130, holder) || !rd(holder + 0x18, model) || !rd(model + 0x12D8 + 0x48, count) || !rd(model + 0x12D8 + 0x50, table))
            return false;
        std::array<char, 64> name{};
        for (const auto& n : names) {
            if (n.kind >= count || !rd(table + n.kind * 0x44, name))
                return false;
            const std::string_view now{name.data(), strnlen(name.data(), name.size())};
            if (now != n.game && now != n.ours)
                return false;
        }
        for (const auto& n : names) {
            name.fill(0);
            const auto want = on ? n.ours : n.game;
            std::memcpy(name.data(), want.data(), want.size());
            host.write_memory(host.userdata, table + n.kind * 0x44, name.data(), name.size());
        }
        return true;
    }
    // The attack types' data (OColl.Setup registers them at boot into a table of the heap): 0x38 bytes per type, its
    // damage four u16 at +0. Found by FestivalLv1's entry (0x149: 150 each, then its own bytes) and Festival's next
    // one (200 each), scanned for a few MB a tick; the blast's hit (MahoroaScrewBurstAttack, 0x341) takes its damage
    static constexpr u8 FestivalLv1Head[24]{0x96, 0, 0x96, 0, 0x96, 0, 0x96, 0, 0x00, 0x0F, 0x01, 0x00,
                                            0x00, 0x06, 0xF0, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x48, 0x42};
    static constexpr std::array<u16, 4> BurstDamage{12, 4, 2, 12}, FestivalDamage{150, 150, 150, 150};
    u64 attack_table{}, attack_scan{};
    bool AttackTableValid(const EdenDsmodHostApi& host, u64 table) const {
        u8 head[24]{};
        std::array<u16, 4> next{};
        return table && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, table + 0x149 * 0x38, head, sizeof head) &&
               std::memcmp(head, FestivalLv1Head, sizeof head) == 0 &&
               dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, table + 0x14A * 0x38, next.data(), sizeof next) &&
               next == std::array<u16, 4>{200, 200, 200, 200};
    }
    void ScanAttackTable(const EdenDsmodHostApi& host, int pages) {
        if (AttackTableValid(host, attack_table))
            return;
        attack_table = 0;
        const u64 begin = host.get_heap_begin(host.userdata) & ~u64{0xFFF}, end = host.get_heap_end(host.userdata);
        if (attack_scan < begin || attack_scan >= end)
            attack_scan = begin;
        for (int n = 0; n < pages && attack_scan < end; ++n, attack_scan += 0x1000) {
            const u8* p = host.get_read_pointer(host.userdata, attack_scan, 0x1000);
            if (!p)
                continue;
            for (u32 at = 0; at + 8 <= 0x1000; at += 4)
                if (std::memcmp(p + at, FestivalLv1Head, 8) == 0 && AttackTableValid(host, attack_scan + at - 0x149 * 0x38)) {
                    attack_table = attack_scan + at - 0x149 * 0x38;
                    if (host.log)
                        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod attack table at " + KirbyHex(attack_table)).c_str());
                    return;
                }
        }
    }
    bool BlastDamage(const EdenDsmodHostApi& host, bool on) {
        std::array<u16, 4> now{};
        const u64 at = attack_table + 0x341 * 0x38;
        if (!host.write_memory || !AttackTableValid(host, attack_table) ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, now.data(), sizeof now) || (now != BurstDamage && now != FestivalDamage))
            return false;
        host.write_memory(host.userdata, at, on ? FestivalDamage.data() : BurstDamage.data(), sizeof now);
        return true;
    }
    int blast_phase{}; // 0 idle, 1 patched and X pressed, 2 the dance running
    u64 blast_at{}, blast_still{};
    void BlastStep(const EdenDsmodHostApi& host) {
        const auto log = [&](const std::string& m) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod blast " + m).c_str());
        };
        const auto restore = [&] {
            const bool words = StepPatch(host, BlastPatches, false), names = HeroAnimNames(host, BlastNames, false),
                       damage = BlastDamage(host, false);
            return words && names && damage ? "restored" : "RESTORE FAILED";
        };
        if (blast_phase == 1) {
            if (play_posing)
                blast_phase = 2, blast_at = blast_still = epi_tick, log("started");
            else if (epi_tick - blast_at > 180)
                blast_phase = 0, log(std::string{"not started, "} + restore());
        } else if (blast_phase == 2) {
            blast_still = play_posing ? epi_tick : blast_still;
            if (epi_tick - blast_still > 30 || epi_tick - blast_at > 1500)
                blast_phase = 0, log(std::string{"done, "} + restore());
        }
    }

    // The Arenas' boss panels (Step/Parts BossPanel; three, for the fights with more bosses):
    // [[[root+0x148]+0x70]+0xA30 / 0xA70 / 0xAB0], each with its N_Top (flags +0x58, alpha +0x59) and T_BossName,
    // whose text (UTF-16) is at the pane's +0xD8
    struct ArenaPanel {
        std::u16string name;
        int flags{-1}, alpha{-1};
        float life{-1}; // P_LifeBarN's (P_LifeBarEx's) fill, 0..1, -1 not found
    };
    std::array<ArenaPanel, 3> ArenaPanels(const EdenDsmodHostApi& host) {
        std::array<ArenaPanel, 3> out;
        const auto rd = [&](u64 at, auto& v) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, sizeof v); };
        u64 root{}, a{}, b{};
        if (!rd(host.main_base + 0x16325B8, root) || !rd(root + 0x148, a) || !rd(a + 0x70, b))
            return out;
        static constexpr u32 Slots[]{0xA30, 0xA70, 0xAB0};
        for (std::size_t i = 0; i < 3; ++i) {
            u64 layout{}, vt{};
            if (!rd(b + Slots[i], layout) || !rd(layout, vt) || vt != host.main_base + 0x15A18D0)
                continue;
            std::vector<char> m(0x6000);
            if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, layout, m.data(), m.size()))
                continue;
            for (std::size_t o = 0xB0; o + 16 < m.size(); o += 8) {
                const std::string_view name{&m[o], strnlen(&m[o], 16)};
                if (name == "N_Top" && out[i].flags < 0)
                    out[i].flags = static_cast<u8>(m[o - 0xB0 + 0x58]), out[i].alpha = static_cast<u8>(m[o - 0xB0 + 0x59]);
                // the bar's Gauge animation slides its texture (texture SRT 1, translate S): 0 full, 1 empty
                if ((name == "P_LifeBarN" || name == "P_LifeBarEx") && o - 0xB0 + 0x240 <= m.size()) {
                    float t{};
                    std::memcpy(&t, &m[o - 0xB0 + 0x23C], 4);
                    out[i].life = std::max(out[i].life, std::clamp(1 - t, 0.0f, 1.0f));
                }
                if (name != "T_BossName" || !out[i].name.empty())
                    continue;
                u64 text{};
                char16_t buf[48]{};
                if (rd(layout + o - 0xB0 + 0xD8, text) && text &&
                    dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, text, buf, sizeof buf - 2))
                    out[i].name.assign(buf, std::char_traits<char16_t>::length(buf));
            }
        }
        return out;
    }
    // The rest room's board (Step ArenaRestArea) shows the next round's picture: a texture pattern animation
    // (ArenaRestAreaBossPic_<round>) with one texture, whose pai1 section the board's binding points at:
    // [[[[[root+0x148]+0xA0]+0x68]+0x3BC]+0x1D0] (validated 2026-10-22). A round is named as Step.msbt's
    // Chara<round> labels (a pair or a group of mid-bosses joined by '_', Ex for the True Arena's).
    static constexpr std::pair<std::string_view, std::string_view> ArenaPictures[]{
        {"ArenaBossPic01^u", "Whispy"}, {"ArenaBossPic01Ex^u", "ExWhispy"}, {"ArenaBossPic02^u", "Doubter"},
        {"ArenaBossPic02Ex^u", "ExDoubter"}, {"ArenaBossPic03^u", "FattyBuffer"}, {"ArenaBossPic03Ex^u", "ExFattyBuffer"},
        {"ArenaBossPic04^u", "Goliath"}, {"ArenaBossPic04Ex^u", "ExGoliath"}, {"ArenaBossPic05^u", "GrandLowper"},
        {"ArenaBossPic05Ex^u", "ExGrandLowper"}, {"ArenaBossPic06^u", "MetalGeneral"}, {"ArenaBossPic06Ex^u", "ExMetalGeneral"},
        {"ArenaBossPic07^u", "Landia"}, {"ArenaBossPic07Ex^u", "ExLandia"}, {"ArenaBossPic08^u", "KingsDoo_GigantEdge"},
        {"ArenaBossPic08Ex^u", "ExKingsDoo_ExGigantEdge"}, {"ArenaBossPic09^u", "Zankibble_Moundeath"},
        {"ArenaBossPic09Ex^u", "ExZankibble_ExMoundeath"}, {"ArenaBossPic10^u", "Lowpers"}, {"ArenaBossPic10Ex^u", "ExLowpers"},
        {"ArenaBossPic11^u", "Roah"}, {"ArenaBossPic11Ex^u", "ExRoah"}, {"ArenaBossPic12^u", "MahoroaBattle1"},
        {"ArenaBossPic12Ex^u", "ExMahoroaBattle1"}, {"ArenaBossPic13^u", "MahoroaBattle2"}, {"ArenaBossPic13Ex^u", "ExMahoroaBattle2"},
        {"ArenaBossPic14Ex^u", "HRD3"}, {"ArenaBossPic15Ex^u", "GalactaKnight"}, {"ArenaBossPic16Ex^u", "SparkDoubter"},
        {"ArenaBossPic17Ex^u", "VolgaBuffer"}, {"ArenaBossPic18Ex^u", "Hydriath"}, {"ArenaBossPic19Ex^u", "DeathLowpers"},
        {"ArenaBossPic20Ex^u", "GrandDethLowper"}, {"ArenaBossPic21Ex^u", "MasterCrown"}};
    std::string ArenaNextRound(const EdenDsmodHostApi& host) {
        const auto rd = [&](u64 at, u64& v) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8); };
        u64 p{};
        if (!rd(host.main_base + 0x16325B8, p))
            return {};
        for (const u32 off : {0x148u, 0xA0u, 0x68u, 0x3BCu, 0x1D0u})
            if (!p || !rd(p + off, p))
                return {};
        char pai[0x60]{};
        if (!p || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, p, pai, sizeof pai) || std::memcmp(pai, "pai1", 4) != 0)
            return {};
        const std::string_view block{pai, sizeof pai};
        for (const auto& [texture, round] : ArenaPictures)
            if (const auto at = block.find(texture); at != std::string_view::npos && pai[at + texture.size()] == 0)
                return std::string{round};
        return {};
    }

    // the rounds of mid-bosses the board pictures together (ArenaBossPic08/09/10), with the names the user gave them
    struct ArenaGroup {
        std::string_view round;
        std::array<std::string_view, 4> members;
        std::u32string_view name;
    };
    // (members as model folders; the True Arena's are their Ex models)
    static constexpr ArenaGroup ArenaGroups[]{
        {"KingsDoo_GigantEdge", {"GigantEdge", "KingsDoo", "Bonkers"}, U"Miniboss All Star 1"},
        {"Zankibble_Moundeath", {"Zankibble", "MounDeath", "WaterGalboros", "Dubior"}, U"Miniboss All Star 2"},
        {"Lowpers", {"Lowper/Model/LowperLv1", "Lowper/Model/LowperLv2", "Lowper/Model/LowperLv3", "Lowper/Model/LowperLv4"}, U""}};
    // (the True Arena's rounds name each of their bosses Ex...)
    static const ArenaGroup* GroupOf(std::string_view round) {
        std::string plain;
        for (std::size_t a = 0, b; a <= round.size(); a = b + 1) {
            b = std::min(round.find('_', a), round.size());
            std::string_view part = round.substr(a, b - a);
            if (part.starts_with("Ex"))
                part.remove_prefix(2);
            plain += (plain.empty() ? "" : "_") + std::string{part};
        }
        for (const auto& g : ArenaGroups)
            if (plain == g.round)
                return &g;
        return nullptr;
    }
    // a round's model folders: one, or a group's
    static std::vector<std::string> ArenaMembers(std::string_view round) {
        const bool ex = round.starts_with("Ex");
        // a Sphere Doomers' fight: Lowper-<level> one, Lowpers-<level> two from that level
        if (const auto hash = round.find('-'); hash != std::string_view::npos && hash + 1 < round.size()) {
            const int first = round[hash + 1] - '0', n = round.substr(0, hash).ends_with("s") ? 2 : 1;
            std::vector<std::string> out;
            for (int lv = std::max(first, 1); lv < first + n && lv <= 4; ++lv)
                out.push_back("Lowper/Model/LowperLv" + std::to_string(lv) + (ex ? "Ex" : ""));
            return out;
        }
        if (const auto* g = GroupOf(round)) {
            std::vector<std::string> out;
            for (const auto m : g->members)
                if (!m.empty())
                    out.push_back(std::string{m} + (ex ? "Ex" : ""));
            return out;
        }
        // the Rampaging Doomers: four of one
        if (round.ends_with("DeathLowpers"))
            return std::vector<std::string>(4, ArenaFolder(round));
        // a pair fighting together (King Doo EX & Bonkers EX): both
        if (round.find('_') != std::string_view::npos) {
            std::vector<std::string> out;
            for (std::size_t a = 0, b; a < round.size(); a = b + 1) {
                b = std::min(round.find('_', a), round.size());
                out.push_back(ArenaFolder(round.substr(a, b - a)));
            }
            return out;
        }
        return {ArenaFolder(round)};
    }

    // A boss's model folder in gfx/Step/Chara (a pair's first), the True Arena's own model where there is one
    static std::string ArenaFolder(std::string_view round) {
        std::string_view first = round.substr(0, round.find('_'));
        const bool ex = first.starts_with("Ex");
        if (ex)
            first.remove_prefix(2);
        static constexpr std::pair<std::string_view, std::string_view> Folders[]{
            {"Whispy", "WhispyWoods"}, {"Moundeath", "MounDeath"}, {"Lowper", "Lowper/Model/LowperLv1"},
            {"Lowpers", "Lowper/Model/LowperLv1"}, {"GalactaKnight", "GalacticKnight"}, {"VolgaBuffer", "FireFattyBuffer"},
            {"Hydriath", "WaterGoliath"}, {"DeathLowpers", "Lowper/Model/DeathLowpersLv1"},
            {"GrandDethLowper", "GrandDeathLowper"}, {"SPBonkers", "SpBonkers"}, {"Dobior", "Dubior"}};
        std::string folder{first};
        for (const auto& [name, dir] : Folders)
            if (first == name)
                folder = dir;
        static constexpr std::string_view WithEx[]{"WhispyWoods", "Doubter", "GigantEdge", "Goliath", "GrandLowper", "KingsDoo", "Landia",
                                                   "MetalGeneral", "MounDeath", "Roah", "Zankibble", "FattyBuffer", "SpBonkers", "Bonkers",
                                                   "Dubior", "WaterGalboros", "MahoroaBattle1", "MahoroaBattle2"};
        if (ex && (std::find(std::begin(WithEx), std::end(WithEx), folder) != std::end(WithEx) || folder.starts_with("Lowper/Model/Lowper")))
            folder += "Ex";
        return folder;
    }

    // the fight's boss: its HUD name matched to Step.msbt's Chara<name> in the game's language (two Magolors share a
    // name: the round's own one wins)
    static constexpr std::string_view ArenaBosses[]{
        "Lowper", "Whispy", "GigantEdge", "Doubter", "WaterGalboros", "KingsDoo", "Zankibble", "Bonkers", "SPBonkers", "Dobior",
        "GrandLowper", "Moundeath", "MetalGeneral", "FattyBuffer", "Goliath", "Roah", "MahoroaBattle1", "Landia", "MahoroaBattle2",
        "HRD3", "GalactaKnight", "Lowpers", "DeathLowpers", "SparkDoubter", "VolgaBuffer", "Hydriath", "GrandDethLowper", "MasterCrown"};
    // the pairs that fight together, named as one on the HUD
    static constexpr std::string_view ArenaPairs[]{"KingsDoo_Moundeath",       "SPBonkers_Moundeath",     "Zankibble_GigantEdge",
                                                   "ExKingsDoo_ExBonkers",     "ExKingsDoo_ExMoundeath",  "ExSPBonkers_ExMoundeath",
                                                   "ExWaterGalboros_ExDobior", "ExZankibble_ExGigantEdge"};
    std::map<std::u16string, std::vector<std::string>> arena_names; // a HUD name -> its labels, in the game's language
    // (arena_round goes off play; arena_board, the board's last round, outlasts a fight's opening scene to tell the
    // two Magolors apart)
    std::string arena_names_dir, arena_round, arena_board;
    std::vector<std::string> ArenaLabels(const std::u16string& name) {
        if (arena_names_dir != title_dir) {
            arena_names.clear(), arena_names_dir = title_dir;
            if (const auto msbt = romfs("/msg/Ark/" + title_dir + "/Step.msbt"))
                for (const auto base : ArenaBosses)
                    for (const std::string label : {std::string{base}, "Ex" + std::string{base}})
                        if (const auto text = KirbyAssets::MsbtText(*msbt, "Chara" + label))
                            arena_names[std::u16string(text->begin(), text->end())].push_back(label);
            if (const auto msbt = romfs("/msg/Ark/" + title_dir + "/Step.msbt"))
                for (const auto pair : ArenaPairs)
                    if (const auto text = KirbyAssets::MsbtText(*msbt, "Chara" + std::string{pair}))
                        arena_names[std::u16string(text->begin(), text->end())].push_back(std::string{pair});
        }
        const auto it = arena_names.find(name);
        return it != arena_names.end() ? it->second : std::vector<std::string>{};
    }

    // Which boss the page shows: in a rest room the board's next round, in a fight the boss named on the HUD (before
    // its name is up, the round's); nothing in the ability room
    // (a group's bosses come one at a time: before the first one's name is up, none; a boss whose bar is empty goes)
    std::string arena_lives;
    bool arena_fight_on{};
    std::size_t arena_doomer_fights{};
    std::string arena_doomers;
    void ArenaBossStep(const EdenDsmodHostApi& host) {
        std::string label;
        const auto next = play_on ? ArenaNextRound(host) : std::string{};
        if (play_on)
            arena_rest = !next.empty();
        if (!next.empty()) {
            arena_life = -1;
            label = arena_round = arena_board = next;
            arena_doomer_fights = 0, arena_doomers.clear();
        } else if (play_on) {
            const auto panels = ArenaPanels(host);
            float life = -1;
            for (const auto& panel : panels) {
                const auto labels = panel.name.empty() ? std::vector<std::string>{} : ArenaLabels(panel.name);
                if (labels.empty() || panel.life == 0)
                    continue;
                label = labels.front(), life = panel.life;
                for (const auto& l : labels)
                    if (arena_board.find(l) != std::string::npos)
                        label = l;
                break;
            }
            // The Sphere Doomers come one, two together, then one: Lv3 (green), Lv1 and Lv2 (purple, red), Lv4 (silver)
            // (the round may be the first, from the ability room: no board then)
            if (!label.empty()) {
                const std::string ex = label.starts_with("Ex") ? "Ex" : "";
                if (label == ex + "Lowper" || label == ex + "Lowpers") {
                    static constexpr std::string_view Fights[]{"Lowper-3", "Lowpers-1", "Lowper-4"};
                    if (!arena_fight_on && life < 0.5f)
                        arena_doomers = ex + std::string{Fights[std::min<std::size_t>(arena_doomer_fights++, 2)]};
                    if (!arena_doomers.empty())
                        label = arena_doomers;
                }
            }
            arena_fight_on = !label.empty() && life >= 0;
            arena_life = label.empty() ? -1 : life;
            arena_bars = -1;
            for (const auto& panel : panels)
                arena_bars = std::max(arena_bars, panel.life);
            const bool named = std::any_of(panels.begin(), panels.end(), [](const auto& p) { return !p.name.empty(); });
            if (label.empty() && !named && !GroupOf(arena_round))
                label = arena_round;
            if (host.log) {
                std::string lives;
                for (const auto& p : panels)
                    lives += " " + std::to_string(std::lround(p.life * 10));
                if (lives + label != arena_lives)
                    arena_lives = lives + label, host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod arena lives" + lives + " " + label).c_str());
            }
        } else
            arena_round.clear();
        arena_boss = arena_boss_name = label;
    }

    // The stage page cut by the sword (user 2026-10-24), in Story and Extra only: each Ultra Sword hit on Grand
    // Doomer's shield shakes the page; the Ultra Sword's last blows (its ExecFinish: a swing, three quick ones,
    // the last) cut it four times, then break it into shards over the starry sky, which stays until the room
    // changes. Grand Doomer's custom object (vtable main+0x15AEA68) keeps its shield at +0xB0 (4 down to 0). A
    // hero's script context (vtable main+0x15F4A78) holds its anim controller at +0x12D8: the kind at +0x38, and
    // its first word owns the anim player [[owner+0x848]+0x70] (length +0x48, frame +0x4C). The last blows play
    // kind 0x299 from frame 0 (a hit on the shield from 120): the first swing at frame 290, then kinds 0x29C,
    // 0x29D, 0x29E, then 0x299 again from 170 to 290. Magolor's first form (user 2026-10-25) the same, with five
    // plates (custom object vtable main+0x15B4BA0, barrier at +0x1C, hit by any super ability): his last blows
    // stop at frame 292 while the swords push against each other (rate 0.1, the page trembles), the first swing
    // lands when Kirby wins, the last is kind 0x29F hitting at frame 130; the sky stays through his second form's fight.
    // The objects are found by scanning the heap a slice per tick in Nutty Noon's boss stage and in Magolor's room
    // (his name on the boss bar). shatter.txt (gen_manifest.py) gives the cuts and the page's pictures.
    static constexpr u64 GrandDoomerVtable = 0x15AEA68, MagolorVtable = 0x15B4BA0, HeroContextVtable = 0x15F4A78, FxScanSlice = 16 << 20;
    static constexpr u32 FinishKind = 0x299, FinalBlowKind = 0x29F;
    static constexpr std::array<u32, 3> SwingKinds{0x29C, 0x29D, 0x29E};
    static constexpr float SwingFrame = 290, FinalBlowFrame = 130; // (its hit; 125 is the swing's start, held still)
    // Magolor's first cut, after the swords pushed against each other (user 2026-10-25): its streak (gen_manifest.py's
    // FX_WIDE_STREAK) and its opening wider
    static constexpr int FxWideStreak = 400;
    static constexpr float FxWideOpening = 5;
    static constexpr int ShakeTicks = 24, SkyFadeTicks = 20, FxWorld = 4, MagolorWorld = 6; // Nutty Noon, the last world (from 0)
    // the offsets on the stage page the shake adds to (gen_manifest.py's fx_shaken: the others take fx.dx / fx.dy)
    static constexpr std::array<const char*, 7> ShakeX{"bg.dx", "play.sx", "abl.sx", "abl.a.x", "abl.b.x", "opt.lx", "opt.px"};
    static constexpr std::array<const char*, 4> ShakeY{"bg.dy", "play.ty.0", "play.ty.1", "play.ty.2"};
    std::optional<KirbyFx::Plan> fx_plan;
    bool fx_plan_read{}, fx_frozen{}, fx_shield{}, fx_magolor{}, fx_qte{}, fx_wide{};
    enum FxBoss { FxDoomer, FxMagolor } fx_boss{FxDoomer};
    int fx_keep_rooms{}; // rooms the sky outlives (Magolor's: the one of his second form)
    u64 fx_tick{}, fx_room{}, fx_scan_at{}, fx_scan_end{}, fx_scan_after{}, fx_scan_from{};
    std::vector<u64> fx_bosses, fx_heroes, fx_found_bosses, fx_found_heroes;
    u32 fx_cover{~0u};
    int fx_blows{-1}, fx_shot{}, fx_boss_logged{-1}; // the last blows: -1 not begun, 0 begun, n cuts started; the page picture's id
    std::array<u64, 5> fx_cut_at{};   // when each cut's streak started
    std::array<u64, 6> fx_shown_at{}; // when the pieces after each cut, then the shards, showed
    u64 fx_shake_at{}, fx_sky_gone{};
    float fx_shake_amp{};
    int fx_dx{}, fx_dy{};
    // the page as it was when the last blows began, drawn on a worker when its first piece is asked for
    struct FxItem {
        std::string key;
        std::array<int, 4> rect;
        std::uint32_t colour;
        float deg, scale;
        bool image;
    };
    std::mutex fx_mutex, fx_draw_mutex;
    std::vector<FxItem> fx_items;
    std::shared_ptr<const KirbyAssets::Image> fx_page;
    int fx_page_id{};
    // the set of cuts the last blows play, mirrored: one of shatter.txt's sets times four mirrors, dealt from a
    // shuffled bag so every variant comes before any comes again and the set changes each time (user 2026-10-25:
    // the same set three times running looked like one pattern); its unmirrored set gives the widgets' places
    std::shared_ptr<const KirbyFx::Set> fx_set, fx_page_set;
    int fx_set_index{}, fx_last_variant{-1};
    std::vector<int> fx_bag;
    std::mt19937 fx_random{static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ std::random_device{}()};

    int FxNextVariant(int variants) {
        if (fx_bag.empty()) {
            std::vector<int> left(static_cast<std::size_t>(variants));
            std::iota(left.begin(), left.end(), 0);
            std::shuffle(left.begin(), left.end(), fx_random);
            for (int last = fx_last_variant; !left.empty();) {
                auto it = std::ranges::find_if(left, [&](int v) { return last < 0 || v / 4 != last / 4; });
                if (it == left.end())
                    it = left.begin();
                fx_bag.push_back(last = *it);
                left.erase(it);
            }
            std::ranges::reverse(fx_bag);
        }
        const int v = fx_bag.back();
        fx_bag.pop_back();
        return v;
    }

    std::string FxKey(const std::string& rest) const {
        return "fx/" + std::to_string(fx_shot) + "/" + rest;
    }

    // what a page shows now, as its widgets' keys resolve (KirbyFx::PageWidget, the runtime's rules)
    std::vector<FxItem> CaptureItems(const EdenDsmodHostApi& host, const std::vector<KirbyFx::PageWidget>& page) {
        const auto value = [&](const std::string& n) { return n.empty() ? 0 : host.get_i64(host.userdata, n.c_str(), 0); };
        const auto nonzero = [&](const std::string& n) {
            const bool neg = !n.empty() && n[0] == '!';
            const bool on = value(neg ? n.substr(1) : n) != 0;
            return neg ? !on : on;
        };
        std::vector<FxItem> items;
        for (const auto& w : page) {
            if ((!w.gate.empty() && !nonzero(w.gate)) || (!w.need_bind.empty() && !nonzero(w.need_bind)))
                continue;
            if (!w.hide_bind.empty()) {
                const auto hv = value(w.hide_bind);
                if (hv < w.keep_min || hv > w.keep_max || (w.hide_eq_on && hv == w.hide_eq))
                    continue;
            }
            FxItem item{{}, w.rect, w.colour, w.rotate, 1, w.image};
            if (!w.x_bind.empty())
                item.rect[0] += static_cast<int>(static_cast<float>(value(w.x_bind)) * w.x_scale);
            if (!w.y_bind.empty())
                item.rect[1] += static_cast<int>(static_cast<float>(value(w.y_bind)) * w.y_scale);
            if (w.image) {
                const char* bound = w.src_bind.empty() ? nullptr : host.get_text(host.userdata, w.src_bind.c_str());
                item.key = bound ? bound : w.src;
                if (item.key.empty())
                    continue;
                if (const auto t = value(w.tint_bind); !w.tint_bind.empty() && t >= 0 && t < static_cast<std::int64_t>(w.tints.size()))
                    item.colour = w.tints[static_cast<std::size_t>(t)];
                item.deg += static_cast<float>(value(w.rotate_bind));
                if (!w.scale_bind.empty())
                    item.scale = static_cast<float>(value(w.scale_bind)) / 1000;
            }
            items.push_back(std::move(item));
        }
        return items;
    }

    // the page those items make (module pictures only; drawn on a worker), or the part of it in `box`
    KirbyAssets::Image DrawItems(const std::vector<FxItem>& items, std::array<int, 4> box = {0, 0, 1240, 1080}) {
        KirbyAssets::Image page{static_cast<std::uint32_t>(box[2]), static_cast<std::uint32_t>(box[3]),
                                std::vector<std::uint8_t>(std::size_t(box[2]) * static_cast<std::size_t>(box[3]) * 4)};
        constexpr std::string_view Files{"module:kirby:gen/files/"};
        for (auto item : items) {
            item.rect[0] -= box[0], item.rect[1] -= box[1];
            const auto [x, y, w, h] = item.rect;
            if (!item.image) {
                KirbyFx::Fill(page, item.rect, item.colour);
                continue;
            }
            if (!item.key.starts_with(Files))
                continue;
            if (const auto image = FilesImage(std::string_view{item.key}.substr(Files.size())))
                KirbyFx::Draw(page, *image, static_cast<float>(x) + static_cast<float>(w) / 2, static_cast<float>(y) + static_cast<float>(h) / 2,
                              static_cast<float>(w), static_cast<float>(h), item.deg, item.scale, item.colour);
        }
        return page;
    }

    void FxCapture(const EdenDsmodHostApi& host) {
        if (!host.get_i64 || !host.get_text)
            return;
        auto items = CaptureItems(host, fx_plan->page);
        std::scoped_lock lock{fx_mutex};
        fx_items = std::move(items), fx_page.reset(), fx_page_id = fx_shot, fx_page_set = fx_set;
    }

    std::shared_ptr<const KirbyAssets::Image> FxPage(int id) {
        std::scoped_lock draw{fx_draw_mutex};
        std::vector<FxItem> items;
        {
            std::scoped_lock lock{fx_mutex};
            if (fx_page_id != id)
                return nullptr;
            if (fx_page)
                return fx_page;
            items = fx_items;
        }
        auto shared = std::make_shared<const KirbyAssets::Image>(DrawItems(items));
        std::scoped_lock lock{fx_mutex};
        if (fx_page_id == id)
            fx_page = shared;
        return shared;
    }

    // fx/white, fx/<id>/p/<cut>/<i> (the pieces after a cut), fx/<id>/s/<i> (the shards), fx/<id>/t/<cut>/<frame> (a streak)
    std::optional<KirbyAssets::Image> RenderFx(const std::vector<std::string>& f) {
        if (f.size() == 2 && f[1] == "white")
            return KirbyAssets::Image{8, 8, std::vector<std::uint8_t>(8 * 8 * 4, 0xFF)};
        if (f.size() < 4)
            return std::nullopt;
        const auto n = [&](std::size_t k) { return std::atoi(f[k].c_str()); };
        std::shared_ptr<const KirbyFx::Set> set;
        {
            std::scoped_lock lock{fx_mutex};
            if (fx_page_id == n(1))
                set = fx_page_set;
        }
        if (!set)
            return std::nullopt;
        if (f[2] == "w" && f.size() == 4) {
            const auto& cut = set->cuts[0];
            const int length = static_cast<int>(std::lround(std::hypot(cut.b.x - cut.a.x, cut.b.y - cut.a.y)));
            return KirbyFx::Streak(length, FxWideStreak, static_cast<float>(n(3) + 1) / KirbyFx::StreakTicks);
        }
        if (f[2] == "t" && f.size() == 5 && n(3) >= 0 && n(3) < 5) {
            const auto& cut = set->cuts[static_cast<std::size_t>(n(3))];
            const int length = static_cast<int>(std::lround(std::hypot(cut.b.x - cut.a.x, cut.b.y - cut.a.y)));
            return KirbyFx::Streak(length, cut.width, static_cast<float>(n(4) + 1) / KirbyFx::StreakTicks);
        }
        const auto page = FxPage(n(1));
        if (!page)
            return std::nullopt;
        if (f[2] == "p" && f.size() == 5 && n(3) >= 1 && n(3) <= 4) {
            const auto& pieces = set->stages[static_cast<std::size_t>(n(3) - 1)];
            if (n(4) >= 0 && n(4) < static_cast<int>(pieces.size()))
                return KirbyFx::CutOut(*page, pieces[static_cast<std::size_t>(n(4))]);
        }
        if (f[2] == "s" && f.size() == 4 && n(3) >= 0 && n(3) < static_cast<int>(set->shards.size()))
            return KirbyFx::CutOut(*page, set->shards[static_cast<std::size_t>(n(3))]);
        return std::nullopt;
    }

    // the last blows over: the page's pictures forgotten, the sky fading back to the page if it was up
    void FxEnd() {
        if (fx_shown_at[0])
            fx_sky_gone = fx_tick;
        if (fx_set) {
            for (std::size_t k = 0; k < 4; ++k)
                for (std::size_t i = 0; i < fx_set->stages[k].size(); ++i)
                    FilesForget(FxKey("p/" + std::to_string(k + 1) + "/" + std::to_string(i)));
            for (std::size_t i = 0; i < fx_set->shards.size(); ++i)
                FilesForget(FxKey("s/" + std::to_string(i)));
            for (int j = 0; j < 5; ++j)
                for (int t = 0; t < KirbyFx::StreakTicks; ++t)
                    FilesForget(FxKey("t/" + std::to_string(j) + "/" + std::to_string(t)));
            for (int t = 0; t < KirbyFx::StreakTicks; ++t)
                FilesForget(FxKey("w/" + std::to_string(t)));
        }
        {
            std::scoped_lock lock{fx_mutex};
            fx_items.clear(), fx_page.reset(), fx_page_id = 0, fx_page_set.reset();
        }
        fx_blows = -1, fx_cut_at = {}, fx_shown_at = {}, fx_frozen = false, fx_qte = false, fx_wide = false, fx_keep_rooms = 0;
        fx_set.reset();
    }

    void FxStep(const EdenDsmodHostApi& host, bool on) {
        ++fx_tick;
        if (!fx_plan_read) {
            fx_plan_read = true;
            if (const auto text = romfs("file:shatter.txt"))
                fx_plan = KirbyFx::ParsePlan({reinterpret_cast<const char*>(text->data()), text->size()});
            if (host.log && !fx_plan)
                host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod fx: no shatter.txt");
        }
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 root = rd(host.main_base + 0x16325B8), room = rd(rd(root + 0x260) + 0x140);
        const bool page = on && fx_plan;
        on = page && room;
        if (room != fx_room || !on) {
            // Magolor's sky stays through the load to his second form and its fight, until the room after it
            if (page && fx_keep_rooms > 0 && fx_shown_at[0]) {
                if (room != fx_room && room)
                    --fx_keep_rooms;
            } else if (fx_blows >= 0)
                FxEnd();
            fx_room = room, fx_bosses.clear(), fx_heroes.clear(), fx_cover = ~0u, fx_shield = false, fx_qte = false;
            play_boss = -1, fx_magolor = false, fx_boss = FxDoomer, fx_scan_at = fx_scan_end = 0, fx_scan_after = fx_tick + 30;
        }
        if (on && play_boss >= 0 && play_boss != fx_boss_logged && host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod fx: boss room of world " + std::to_string(play_boss)).c_str());
        fx_boss_logged = on ? play_boss : -1;
        // Magolor's first form is told by his name on the boss bar (Step.msbt's Chara<Ex>MahoroaBattle1)
        if (on && play_world == MagolorWorld && !fx_magolor && fx_blows < 0 && fx_tick % 30 == 0)
            for (const auto& panel : ArenaPanels(host))
                for (const auto& label : panel.name.empty() ? std::vector<std::string>{} : ArenaLabels(panel.name))
                    fx_magolor |= label.ends_with("MahoroaBattle1");
        // a slice of the heap per tick, again every few seconds until the boss is there
        if (on && (play_boss == FxWorld || fx_magolor) && fx_bosses.empty() && fx_blows < 0 && !fx_scan_end && fx_tick >= fx_scan_after) {
            fx_scan_at = host.get_heap_begin(host.userdata) & ~u64{0xFFF}, fx_scan_end = host.get_heap_end(host.userdata);
            fx_found_bosses.clear(), fx_found_heroes.clear(), fx_scan_from = fx_tick;
        }
        if (on && fx_scan_end) {
            const u64 doomer = host.main_base + GrandDoomerVtable, magolor = host.main_base + MagolorVtable,
                      hero = host.main_base + HeroContextVtable;
            for (const u64 stop = std::min(fx_scan_end, fx_scan_at + FxScanSlice); fx_scan_at < stop; fx_scan_at += 0x1000) {
                const u8* q = host.get_read_pointer(host.userdata, fx_scan_at, 0x1000);
                if (!q)
                    continue;
                for (u32 at = 0; at < 0x1000; at += 8) {
                    u64 v;
                    std::memcpy(&v, q + at, 8);
                    if (v == doomer || v == magolor)
                        fx_found_bosses.push_back(fx_scan_at + at);
                    else if (v == hero)
                        fx_found_heroes.push_back(fx_scan_at + at);
                }
            }
            if (fx_scan_at >= fx_scan_end) {
                fx_bosses = fx_found_bosses, fx_heroes = fx_found_heroes, fx_scan_end = 0, fx_scan_after = fx_tick + 180;
                if (host.log)
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                             ("Kirby DSMod fx: heap scanned in " + std::to_string(fx_tick - fx_scan_from) + " ticks, " +
                              std::to_string(fx_bosses.size()) + " boss(es), " + std::to_string(fx_heroes.size()) + " hero(es)")
                                 .c_str());
            }
        }
        // a hit on the shield (Grand Doomer's four plates, Magolor's five) shakes the page, the one that breaks it harder
        u32 cover = ~0u;
        for (const u64 b : fx_bosses) {
            const u64 vt = rd(b);
            if (vt == host.main_base + GrandDoomerVtable)
                cover = std::min(cover, static_cast<u32>(rd(b + 0xB0))), fx_boss = FxDoomer;
            else if (vt == host.main_base + MagolorVtable)
                cover = std::min(cover, static_cast<u32>(rd(b + 0x1C))), fx_boss = FxMagolor;
        }
        if (cover < fx_cover && fx_cover <= 5)
            fx_shake_at = fx_tick, fx_shake_amp = cover == 0 ? 30.0f : 20.0f;
        fx_cover = cover;
        fx_shield |= cover >= 1 && cover <= 5;
        // the last blows, read from the hero playing them
        for (const u64 h : fx_heroes) {
            if (rd(h) != host.main_base + HeroContextVtable)
                continue;
            const u64 ctl = h + 0x12D8, z = rd(rd(rd(ctl) + 0x848) + 0x70);
            const u32 kind = static_cast<u32>(rd(ctl + 0x38));
            const auto swing = std::ranges::find(SwingKinds, kind);
            float player[2]{}; // frame, rate
            if ((kind != FinishKind && kind != FinalBlowKind && swing == SwingKinds.end()) || !z ||
                !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, z + 0x4C, player, sizeof player))
                continue;
            const float frame = player[0], rate = player[1];
            const bool magolor = fx_boss == FxMagolor;
            int cut = -1;
            if (fx_blows < 0 && fx_shield && kind == FinishKind && frame < 100 && cover == 0) {
                // they begin: the page as it is now (its background held still from here) is drawn for its pieces
                const int variant = FxNextVariant(static_cast<int>(fx_plan->sets.size()) * 4);
                fx_last_variant = variant, fx_set_index = variant / 4;
                fx_set = std::make_shared<const KirbyFx::Set>(
                    KirbyFx::Mirror(fx_plan->sets[static_cast<std::size_t>(fx_set_index)], variant & 1, variant & 2));
                fx_blows = 0, ++fx_shot, fx_frozen = true, fx_keep_rooms = magolor ? 1 : 0, fx_wide = magolor;
                FxCapture(host);
                for (std::size_t k = 0; k < 4; ++k)
                    for (std::size_t i = 0; i < fx_set->stages[k].size(); ++i)
                        FilesAhead(FxKey("p/" + std::to_string(k + 1) + "/" + std::to_string(i)));
                for (std::size_t i = 0; i < fx_set->shards.size(); ++i)
                    FilesAhead(FxKey("s/" + std::to_string(i)));
                for (int j = 0; j < 5; ++j)
                    for (int t = 0; t < KirbyFx::StreakTicks; ++t)
                        FilesAhead(FxKey((j == 0 && fx_wide ? "w/" : "t/" + std::to_string(j) + "/") + std::to_string(t)));
                if (host.log)
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                             ("Kirby DSMod fx: the last blows begin (" + std::string{magolor ? "Magolor" : "Grand Doomer"} + "), set " +
                              std::to_string(fx_set_index) + " mirror " + std::to_string(variant % 4))
                                 .c_str());
            } else if (fx_blows == 0 && kind == FinishKind && frame >= SwingFrame) {
                // Magolor's: the swords pushed against each other (the swing slowed down) until Kirby wins, then it lands
                if (!magolor)
                    cut = 0;
                else if (rate < 0.3f && frame < 296)
                    fx_qte = true;
                else if (fx_qte)
                    fx_qte = false, cut = 0;
            } else if (fx_blows >= 1 && fx_blows <= 3 && swing != SwingKinds.end())
                cut = static_cast<int>(swing - SwingKinds.begin()) + 1;
            else if (fx_blows == 4 && (magolor ? kind == FinalBlowKind && frame >= FinalBlowFrame : kind == FinishKind && frame >= SwingFrame))
                cut = 4;
            // (a swing missed is cut with the next one)
            for (; cut >= 0 && fx_blows <= cut; ++fx_blows) {
                fx_cut_at[static_cast<std::size_t>(fx_blows)] = fx_tick;
                if (host.log)
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod fx: cut " + std::to_string(fx_blows)).c_str());
            }
            break;
        }
        FxPublish(host);
    }

    void FxPublish(const EdenDsmodHostApi& host) {
        const auto pub = [&](const std::string& n, std::int64_t v) { host.publish_i64(host.userdata, n.c_str(), v); };
        const auto turn = [&](const std::string& n, float deg) { host.publish_f64(host.userdata, n.c_str(), deg); };
        const auto text = [&](const std::string& n, const std::string& key) {
            host.publish_text(host.userdata, n.c_str(), ("module:kirby:gen/files/" + key).c_str());
        };
        const auto alpha = [](float a) { return static_cast<std::int64_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 15)); };
        const auto since = [&](u64 at) { return static_cast<float>(fx_tick - at); };
        // the shake: a fresh offset every other tick, dying away; each cut jolts the pieces
        float sx = 0, sy = 0, jx = 0, jy = 0;
        const auto jolt = [&](u64 at, float amp, int ticks, float& x, float& y) {
            if (!at || fx_tick < at || fx_tick >= at + static_cast<u64>(ticks))
                return;
            const float k = amp * (1 - since(at) / static_cast<float>(ticks));
            const u64 step = (fx_tick - at) / 2 + at;
            x += k * std::sin(static_cast<float>(step) * 2.7f), y += k * std::cos(static_cast<float>(step) * 3.1f);
        };
        jolt(fx_shake_at, fx_shake_amp, ShakeTicks, sx, sy);
        // the whole time the swords push against each other the page trembles
        if (fx_qte) {
            const float step = static_cast<float>(fx_tick / 2);
            sx += 9 * std::sin(step * 2.7f), sy += 9 * std::cos(step * 3.1f);
        }
        for (int j = 0; j < 5; ++j)
            if (fx_cut_at[static_cast<std::size_t>(j)])
                jolt(fx_cut_at[static_cast<std::size_t>(j)] + KirbyFx::StreakTicks, j == 4 ? 18.0f : 10.0f, 10, jx, jy);
        fx_dx = static_cast<int>(std::lround(sx)), fx_dy = static_cast<int>(std::lround(sy));
        pub("fx.dx", fx_dx), pub("fx.dy", fx_dy);
        // the pieces after a cut show once its streak has crossed and their pictures are with the host; the shards
        // after the last blow's cracks have shown for a moment
        const auto served = [&](std::size_t stage) {
            if (!fx_set)
                return false;
            if (stage == 5) {
                for (std::size_t i = 0; i < fx_set->shards.size(); ++i)
                    if (!FilesServed(FxKey("s/" + std::to_string(i))))
                        return false;
                return true;
            }
            for (std::size_t i = 0; i < fx_set->stages[stage - 1].size(); ++i)
                if (!FilesServed(FxKey("p/" + std::to_string(stage) + "/" + std::to_string(i))))
                    return false;
            return true;
        };
        for (std::size_t k = 0; k < 5; ++k)
            if (fx_cut_at[k] && !fx_shown_at[k] && fx_tick >= fx_cut_at[k] + KirbyFx::StreakTicks && served(k + 1))
                fx_shown_at[k] = fx_tick;
        int stage = 0;
        for (int k = 0; k < 5; ++k)
            if (fx_shown_at[static_cast<std::size_t>(k)])
                stage = k + 1;
        const float shards = stage == 5 ? since(fx_shown_at[4]) - KirbyFx::CrackTicks : 0;
        if (stage == 5 && shards >= KirbyFx::ShardTicks)
            stage = 6;
        pub("fx.show", stage >= 1 && stage <= 5 ? fx_set_index * 8 + stage : 0);
        pub("fx.on", stage > 0 ? 1 : 0);
        const float sky = stage > 0 ? 1 : fx_sky_gone ? 1 - since(fx_sky_gone) / SkyFadeTicks : 0;
        pub("fx.sky", alpha(sky));
        if (sky <= 0)
            fx_sky_gone = 0;
        if (fx_blows < 0 || !fx_set)
            return;
        // a mirrored piece is drawn by its unmirrored widget, moved to its own box
        const auto& base = fx_plan->sets[static_cast<std::size_t>(fx_set_index)];
        const auto moved = [](const KirbyFx::Shape& to, const KirbyFx::Shape& from) {
            return std::array<float, 2>{static_cast<float>(to.box[0] - from.box[0]), static_cast<float>(to.box[1] - from.box[1])};
        };
        // every picture handed to the host ahead, from the last blows' start until the shards fly
        const bool ahead = stage < 5;
        pub("fx.preload", ahead ? 1 : 0);
        for (std::size_t k = 0; k < 4; ++k)
            for (std::size_t i = 0; i < fx_set->stages[k].size(); ++i) {
                const std::string n = "fx.p." + std::to_string(k + 1) + "." + std::to_string(i);
                if (const auto key = FxKey("p/" + std::to_string(k + 1) + "/" + std::to_string(i));
                    stage == static_cast<int>(k) + 1 || (ahead && FilesDone(key)))
                    text(n, key);
                if (stage != static_cast<int>(k) + 1)
                    continue;
                const auto place = KirbyFx::PiecePlace(*fx_set, static_cast<int>(k), i, since(fx_shown_at[k]), fx_wide ? FxWideOpening : 1);
                const auto [mx, my] = moved(fx_set->stages[k][i], base.stages[k][i]);
                pub(n + ".x", std::lround(place.dx + mx + jx)), pub(n + ".y", std::lround(place.dy + my + jy)), turn(n + ".r", place.deg);
            }
        for (std::size_t i = 0; i < fx_set->shards.size(); ++i) {
            const std::string n = "fx.s." + std::to_string(i);
            if (const auto key = FxKey("s/" + std::to_string(i)); stage == 5 || (ahead && FilesDone(key)))
                text(n, key);
            if (stage != 5)
                continue;
            const auto place = KirbyFx::ShardPlace(*fx_set, i, shards, fx_wide ? FxWideOpening : 1);
            const auto [mx, my] = moved(fx_set->shards[i], base.shards[i]);
            pub(n + ".x", std::lround(place.dx + mx + jx)), pub(n + ".y", std::lround(place.dy + my + jy)), turn(n + ".r", place.deg);
            pub(n + ".s", std::lround(place.scale * 1000)), pub(n + ".a", alpha(place.alpha));
        }
        // the streaks cross the page, then fade (a mirrored one moved and turned onto its cut)
        for (int j = 0; j < 5; ++j) {
            const u64 at = fx_cut_at[static_cast<std::size_t>(j)];
            const bool wide = j == 0 && fx_wide;
            const std::string n = wide ? "fx.w." + std::to_string(fx_set_index) : "fx.t." + std::to_string(fx_set_index) + "." + std::to_string(j);
            const auto streak = [&](int t) { return FxKey((wide ? "w/" : "t/" + std::to_string(j) + "/") + std::to_string(t)); };
            for (int t = 0; ahead && t < KirbyFx::StreakTicks; ++t)
                if (const auto key = streak(t); FilesDone(key))
                    text((wide ? "fx.prew." : "fx.pre." + std::to_string(j) + ".") + std::to_string(t), key);
            const float t = at ? since(at) : -1;
            if (t < 0 || t >= KirbyFx::StreakTicks + 14) {
                pub(n + ".a", 0);
                continue;
            }
            text(n, streak(std::min(static_cast<int>(t), KirbyFx::StreakTicks - 1)));
            pub(n + ".a", alpha(t < KirbyFx::StreakTicks ? 1 : 1 - (t - KirbyFx::StreakTicks) / 14));
            const auto& to = fx_set->cuts[static_cast<std::size_t>(j)];
            const auto& from = base.cuts[static_cast<std::size_t>(j)];
            const auto c = KirbyFx::LineCentre(to), c0 = KirbyFx::LineCentre(from);
            pub(n + ".x", std::lround(c.x - c0.x)), pub(n + ".y", std::lround(c.y - c0.y));
            turn(n + ".r", std::remainder(KirbyFx::LineDegrees(to) - KirbyFx::LineDegrees(from), 360.0f));
        }
        // the last blow's flash
        const float flash = fx_shown_at[4] ? 0.8f * (1 - since(fx_shown_at[4]) / 14) : fx_cut_at[4] && since(fx_cut_at[4]) >= KirbyFx::StreakTicks ? 0.8f : 0;
        pub("fx.flash", alpha(flash));
    }

    // the shake on the offsets other steps published this tick (fx.dx / fx.dy carry it for the rest)
    void FxShakeOffsets(const EdenDsmodHostApi& host) {
        if ((fx_dx == 0 && fx_dy == 0) || !host.get_i64)
            return;
        constexpr std::int64_t None = INT64_MIN;
        for (const char* n : ShakeX)
            if (const auto v = host.get_i64(host.userdata, n, None); v != None)
                host.publish_i64(host.userdata, n, v + fx_dx);
        for (const char* n : ShakeY)
            if (const auto v = host.get_i64(host.userdata, n, None); v != None)
                host.publish_i64(host.userdata, n, v + fx_dy);
    }

    // How long a figure's loop of ArenaFrames pictures lasts (arenakirby/<ability>/, arenaboss/<round>/): its idle's
    // length, a frame a tick, squeezed to half at most and never under 24 ticks (user 2026-10-24: idles were all played
    // in 24 ticks, Magolor's 200 frames eight times too fast); a group's by its longest idle. Read once its first
    // picture is drawn (its motions are loaded then).
    std::unordered_map<std::string, int> arena_loop_ticks;
    std::deque<std::string> arena_recent;
    int ArenaLoopTicks(const std::string& base) {
        if (const auto it = arena_loop_ticks.find(base); it != arena_loop_ticks.end())
            return it->second;
        if (!FilesDone(base + "0"))
            return 24;
        const auto slash = base.find('/');
        const std::string kind = base.substr(0, slash), name = base.substr(slash + 1, base.size() - slash - 2);
        int frames = 24; // Kirby's StandReadyR
        if (kind == "arenakirby") {
            if (const std::string folder{KirbyChara::CharacterFolder(name)}; !folder.empty())
                if (const auto motion = CharaFile(chara_motions, "gfx/Step/Chara/" + folder + "/Motion"))
                    frames = motion->Frames("Wait");
        } else {
            frames = 0;
            for (const auto& member : ArenaMembers(name))
                if (const auto [motion, anim] = BossIdle(member); motion && !anim.empty())
                    frames = std::max(frames, motion->Frames(anim));
        }
        return arena_loop_ticks[base] = std::max(24, frames / 2);
    }

    // The Arenas' page: on its title (no stage HUD yet) only the background, the plain Kirby's frames drawn ahead
    // meanwhile; in play the bar, Kirby's loop with his ability and, once known, the boss's.
    // Kirby's colour (the colour menu, user's sketch 2026-10-28). The preset is kept in seven persisted flags (options from
    // ColourBit) and put on Kirby's model wherever the game keeps it (KirbyColour::PresetWrites): found at a page's start by
    // its marks (looked for while Kirby's page is up, a slice of the heap a tick), checked twice a second and written again
    // when the game loads it afresh. Only the model's own words or ours are ever written over.
    static constexpr int ColourBits = 7;
    std::vector<u8> colour_model; // the model's file, decompressed once for its words
    std::vector<std::vector<KirbyColour::ModelWrite>> colour_writes;
    bool colour_model_read{}, colour_known{};
    u64 colour_base{}, colour_cursor{}, colour_refused{}, colour_tick{};
    int colour_want{}, colour_written{-1}, colour_file{-1};
    bool colour_storing{};
    // Research: a colour to try (user:colourtest.txt, "skin=RRGGBB cheeks=RRGGBB shoes=RRGGBB [outline=RRGGBB] [eyes=RRGGBB] [eyestop=RRGGBB] [highlight=RRGGBB] [rainbow=1]"), put on in place of the
    // file's while the file holds one (read every second, not stored)
    std::optional<KirbyColour::Preset> colour_test;
    std::vector<KirbyColour::ModelWrite> colour_test_writes;
    std::string colour_test_text;
    int colour_test_version{};
    u64 colour_test_poll{};

    void ColourTestPoll() {
        const auto bytes = romfs("user:colourtest.txt");
        const std::string text = bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
        if (text == colour_test_text)
            return;
        colour_test_text = text;
        const auto hex = [&](std::string_view key) -> std::optional<KirbyColour::Srgb> {
            const auto at = text.find(key);
            if (at == std::string::npos || at + key.size() + 6 > text.size())
                return std::nullopt;
            const auto v = std::strtoul(text.substr(at + key.size(), 6).c_str(), nullptr, 16);
            return KirbyColour::Srgb{static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
        };
        const auto skin = hex("skin="), cheeks = hex("cheeks="), shoes = hex("shoes=");
        colour_test.reset(), colour_test_writes.clear();
        if (skin && cheeks && shoes) {
            colour_test = KirbyColour::Preset{"test", U"Prova", U"Test", KirbyColour::Path::Custom, *skin, *cheeks, *shoes, *skin, hex("outline="),
                                              hex("eyes="), hex("eyestop="), hex("highlight="), text.find("rainbow=1") != std::string::npos};
            ++colour_test_version;
        }
    }

    // A puff of smoke around Kirby as a colour goes on, the colour changing under the cloud. A Basil script calls only
    // the natives its own refs list: ChangeEasyMode's Exec (run only when Helper mode is switched) is rewritten to
    // request the effect on its hero when it is Kirby and return false; its requestN(Kind, uint) ref is swapped for the
    // native requestN(Kind, uint, float scale) of Magolor's dash attack script. Calls Kirby's anim scripts make often (a ref
    // of theirs, a {method, type} pair in the type's resolved table) point at it, all until the puff shows in Kirby's
    // list of effects. Standing still, the idle loops call Wait's ref 33 (IsCancelSpecialMotion) in place of the
    // native IsPinch() they run every frame (IsCancelSpecialMotion itself is skipped while the game has no input
    // focus, and in the first idle cycle); falling, landing and swimming, Utility's loops call its ref 27 (ResetFlip)
    // in place of VacuumCount() (0, as with an empty mouth). Crouching held and guarding run no script, dodging only
    // natives: Kirby puffs as he leaves them.
    static constexpr u32 WaitScript = 0x273324, UtilityScript = 0x272340, EasyScript = 0x26b4c4, EasyExec = EasyScript + 0x1f4;
    struct SmokeCall {
        u32 script, ref, callee; // the anim script's blob, its ref, the callee's code (ScnStep.bin offsets)
    };
    static constexpr u32 WaitTillAnimFrame = 0x23d080, WaitTillIsAnimEnd = 0x23d118, OmenLocatorAnimStart = 0x2e4f98;
    static constexpr SmokeCall SmokeCalls[]{
        {WaitScript, 33, WaitScript + 0x778}, // standing still: every frame
        {0x27411c, 19, WaitTillAnimFrame},   // Walk: a few times a step (a wait skipped once)
        {0x270e88, 25, WaitTillAnimFrame},   // Run
        {0x26f754, 15, 0x26fa04},            // Jump: SetHeadbuttAttack, every frame
        {0x26fc14, 6, OmenLocatorAnimStart}, // JumpStart, Hover, HoverStart, Fall, Landing: as they start
        {0x26ef24, 6, OmenLocatorAnimStart},
        {0x26f504, 4, OmenLocatorAnimStart},
        {0x26e2ec, 10, OmenLocatorAnimStart},
        {0x270114, 13, OmenLocatorAnimStart},
        {UtilityScript, 27, UtilityScript + 0x504}, // the falling, landing and swimming loops (sites below)
        {0x271d14, 11, WaitTillIsAnimEnd},         // Squat, Sliding, AirJump: as they start (the start's wait skipped)
        {0x2714a8, 11, WaitTillIsAnimEnd},
        {0x26a940, 7, WaitTillIsAnimEnd},
    };
    static constexpr u32 DashScript = 0x2a9ac8, DashRequest = 28, EasyRequest = 14;
    static std::array<StepWord, 18> SmokeWords(u32 kind, u32 node, float scale) {
        return {{
            {EasyExec + 0x04, 0x00ffffbf, 0x00ffffbe},
            {EasyExec + 0x08, 0x00000209, 0x00010011}, // r0 = Obj()
            {EasyExec + 0x0c, 0x00000509, 0xff000201},
            {EasyExec + 0x10, 0x00010711, 0x00120111}, // r1 = r2.isKirby()
            {EasyExec + 0x14, 0xff070001, 0x00090119}, // not Kirby: return false
            {EasyExec + 0x18, 0xff070901, 0xff000201},
            {EasyExec + 0x1c, 0x00020811, 0x000c0111}, // r1 = r2.effect()
            {EasyExec + 0x20, 0xff080a01, 0xff010301},
            {EasyExec + 0x24, 0x00030911, 0x000d0211}, // r2 = r3.normal()
            {EasyExec + 0x28, 0xff090101, 0x0010030b},
            {EasyExec + 0x2c, 0xff080b01, 0x0014040b},
            {EasyExec + 0x30, 0x00040a11, 0x0008050d},
            {EasyExec + 0x34, 0x00000508, 0x000e0212}, // r2.requestN(const 0x10, const 0x14, const 0x8)
            {EasyExec + 0x38, 0xff050b01, 0x0000000b},
            {EasyExec + 0x3c, 0x00000c0d, 0xffff0015}, // return false
            {EasyScript + 0x34, 0x3f59999a, std::bit_cast<u32>(scale)},
            {EasyScript + 0x3c, 0x00000482, kind},
            {EasyScript + 0x40, 0x00000002, node},
        }};
    }
    static constexpr StepWord SmokeSites[]{
        {WaitScript + 0x5ac, 0x00130c11, 0x00210c0f},
        {WaitScript + 0x690, 0x00130c11, 0x00210c0f},
        {WaitScript + 0x73c, 0x00130c11, 0x00210c0f},
        {UtilityScript + 0x374, 0x00100111, 0x001b010f},
        {UtilityScript + 0x3c4, 0x00100111, 0x001b010f},
        {UtilityScript + 0x438, 0x00100111, 0x001b010f},
    };
    // EnemyCommonEraseSmoke on the body; the cloud hides Kirby from about 6 to 15 frames after the request. A Kirby
    // who does not stand still within SmokeWait ticks changes colour with no puff.
    static constexpr u32 SmokeKind = 371, SmokeNode = 2, SmokeCover = 7, SmokeWait = 600;
    static constexpr float SmokeScale = 1.4f;
    u64 smoke_easy_type{}, smoke_easy_exec{}, smoke_dash_type{}, smoke_request{}, smoke_scan{}, smoke_retry{}, smoke_tick{}, smoke_on{};
    std::array<u64, std::size(SmokeCalls)> smoke_types{};                    // each anim script's type object
    std::array<std::array<u64, 2>, std::size(SmokeCalls)> smoke_game{};      // the game's pairs while hooked
    int smoke_phase{}; // 0 idle, 1 wanted, 2 hooked, 3 puffed (or given up: unhooked, the code put back a little later)
    u32 smoke_kind{SmokeKind}, smoke_node{SmokeNode};
    float smoke_scale{SmokeScale};
    std::array<StepWord, 18> smoke_words{}; // as written, for putting back
    std::vector<u64> smoke_known; // the effects Kirby had when hooked
    // the colour left on Kirby until the cloud covers him
    int colour_held{-1};
    std::string smoke_test_text;
    bool smoke_test_read{};

    u64 SmokeRead(const EdenDsmodHostApi& host, u64 at) const {
        u64 v{};
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
    }
    bool SmokeReady(const EdenDsmodHostApi& host) const {
        if (!step_file || SmokeRead(host, smoke_easy_type + 8) != step_file + EasyScript || SmokeRead(host, smoke_easy_exec + 0x20) != step_file + EasyExec ||
            SmokeRead(host, smoke_dash_type + 8) != step_file + DashScript)
            return false;
        for (std::size_t i = 0; i < std::size(SmokeCalls); ++i)
            if (SmokeRead(host, smoke_types[i] + 8) != step_file + SmokeCalls[i].script)
                return false;
        return true;
    }

    // the type objects ([+8] = the blob) and Exec's method object ([+0x20] = its code) are found once a boot, a
    // millisecond's worth of heap a tick from the first stage on
    void SmokeSearch(const EdenDsmodHostApi& host) {
        if (!play_on || smoke_tick < smoke_retry || (smoke_easy_type && SmokeReady(host)) || !FindStepFile(host))
            return;
        const u64 lo = host.get_heap_begin(host.userdata), hi = host.get_heap_end(host.userdata);
        if (!smoke_scan)
            smoke_scan = lo & ~u64{0xFFF}, smoke_easy_type = smoke_easy_exec = smoke_dash_type = 0, smoke_types = {};
        const u64 easy = step_file + EasyScript, exec = step_file + EasyExec, dash = step_file + DashScript;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        for (int n = 0; smoke_scan < hi; smoke_scan += 0x1000) {
            if (++n % 64 == 0 && std::chrono::steady_clock::now() > until)
                return;
            const u8* q = host.get_read_pointer(host.userdata, smoke_scan, 0x1000);
            if (!q)
                continue;
            bool hit = false;
            for (u32 o = 0; o < 0x1000; o += 8) {
                u64 v;
                std::memcpy(&v, q + o, 8);
                if (v < step_file || v - step_file >= 0x300000)
                    continue;
                if (v == easy)
                    smoke_easy_type = smoke_scan + o - 8, hit = true;
                else if (v == exec)
                    smoke_easy_exec = smoke_scan + o - 0x20, hit = true;
                else if (v == dash)
                    smoke_dash_type = smoke_scan + o - 8, hit = true;
                for (std::size_t i = 0; i < std::size(SmokeCalls); ++i)
                    if (v == step_file + SmokeCalls[i].script)
                        smoke_types[i] = smoke_scan + o - 8, hit = true;
            }
            if (hit && SmokeReady(host))
                break;
        }
        smoke_scan = 0;
        const bool ready = SmokeReady(host);
        if (!ready)
            smoke_retry = smoke_tick + 600;
        if (host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ready ? "Kirby DSMod smoke ready" : "Kirby DSMod smoke objects not found");
    }

    // ChangeEasyMode's requestN(Kind, uint) native (descriptor hash at +0x10) for the scaled one, and back
    bool SmokeScaled(const EdenDsmodHostApi& host, bool on) {
        const u64 table = SmokeRead(host, smoke_easy_type + 0x48), at = table + EasyRequest * 16, entry = table ? SmokeRead(host, at) : 0;
        const u64 dash_table = SmokeRead(host, smoke_dash_type + 0x48), scaled = dash_table ? SmokeRead(host, dash_table + DashRequest * 16) : 0;
        const auto hash = [&](u64 native) { return static_cast<u32>(SmokeRead(host, native + 0x10)); };
        if (!entry || !scaled || hash(scaled) != 0xf750ea6c)
            return false;
        if (entry != scaled) {
            if (hash(entry) != 0xb22237da)
                return false;
            smoke_request = entry;
        }
        const u64 want = on ? scaled : smoke_request;
        if (want && want != entry)
            host.write_memory(host.userdata, at, &want, 8);
        return want != 0;
    }

    // every call or none; a pair is put back only where ours still is
    bool SmokeHook(const EdenDsmodHostApi& host, bool on) {
        std::array<u64, std::size(SmokeCalls)> at{};
        for (std::size_t i = 0; i < std::size(SmokeCalls); ++i) {
            const u64 table = SmokeRead(host, smoke_types[i] + 0x48);
            u64 entry[2]{};
            if (!table || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, table + SmokeCalls[i].ref * 16, entry, 16)) {
                if (on)
                    return false;
                continue;
            }
            at[i] = table + SmokeCalls[i].ref * 16;
            const bool ours = entry[0] == smoke_easy_exec && entry[1] == smoke_easy_type;
            if (on && !ours) {
                if (SmokeRead(host, entry[0] + 0x20) != step_file + SmokeCalls[i].callee)
                    return false;
                smoke_game[i] = {entry[0], entry[1]};
            }
            if (!on && !ours)
                at[i] = 0;
        }
        for (std::size_t i = 0; i < std::size(SmokeCalls); ++i) {
            const u64 ours[2]{smoke_easy_exec, smoke_easy_type};
            if (at[i] && (on || smoke_game[i][0]))
                host.write_memory(host.userdata, at[i], on ? ours : smoke_game[i].data(), 16);
        }
        return true;
    }

    // research: user:smoketest.txt "<n> [kind=K] [node=N] [scale=S]", a new n puffs (read every half second)
    void SmokeTestPoll(const EdenDsmodHostApi& host) {
        const auto bytes = romfs("user:smoketest.txt");
        const std::string text = bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
        const bool first = !smoke_test_read;
        smoke_test_read = true;
        if (text == smoke_test_text)
            return;
        smoke_test_text = text;
        const auto num = [&](std::string_view key, u32 fallback) {
            const auto at = text.find(key);
            return at == std::string::npos ? fallback : static_cast<u32>(std::strtoul(text.c_str() + at + key.size(), nullptr, 0));
        };
        smoke_kind = num("kind=", SmokeKind), smoke_node = num("node=", SmokeNode);
        const auto scale = text.find("scale=");
        smoke_scale = scale == std::string::npos ? SmokeScale : std::strtof(text.c_str() + scale + 6, nullptr);
        if (!text.empty() && !first)
            Smoke(host);
    }

    // the effects requested through Kirby's hero.effect().normal(): a list at E+0x38 (E = [P+0x140], [E+8] = P), empty
    // while he stands still
    std::optional<std::vector<u64>> SmokeEffects(const EdenDsmodHostApi& host) const {
        const u64 root = SmokeRead(host, host.main_base + 0x16325B8), rec = root ? SmokeRead(host, root + 0x260) : 0,
                  p = rec ? SmokeRead(host, rec + 0x140) : 0, e = p ? SmokeRead(host, p + 0x140) : 0;
        if (!e || SmokeRead(host, e + 8) != p)
            return std::nullopt;
        std::vector<u64> out;
        for (u64 n = SmokeRead(host, e + 0x38); n && n != e + 0x38 && out.size() < 32; n = SmokeRead(host, n))
            out.push_back(n);
        return out;
    }

    // false: no puff (the colour goes on at once); true also when one is already on its way
    bool Smoke(const EdenDsmodHostApi& host) {
        if (smoke_phase == 1 || smoke_phase == 2)
            return true;
        if (smoke_phase != 0 || !play_on || !host.write_memory || !SmokeReady(host))
            return false;
        smoke_phase = 1;
        return true;
    }

    void SmokeStep(const EdenDsmodHostApi& host) {
        const auto log = [&](const char* m) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, (std::string("Kirby DSMod smoke ") + m).c_str());
        };
        ++smoke_tick;
        if (smoke_tick % 30 == 0)
            SmokeTestPoll(host);
        SmokeSearch(host);
        if (smoke_phase == 1)
            smoke_words = SmokeWords(smoke_kind, smoke_node, smoke_scale);
        const auto& words = smoke_words;
        const auto unhook = [&](const char* why) {
            smoke_phase = 3, smoke_on = smoke_tick;
            const bool sites = StepPatch(host, SmokeSites, false), hook = SmokeHook(host, false);
            log(sites && hook ? why : "UNHOOK FAILED");
        };
        if (smoke_phase == 1) {
            const auto effects = SmokeEffects(host);
            if (!effects)
                smoke_phase = 0, colour_held = -1, log("no Kirby");
            else if (!StepPatch(host, words, true))
                smoke_phase = 0, colour_held = -1, log("script refused");
            else if (!SmokeScaled(host, true))
                smoke_phase = 0, colour_held = -1, StepPatch(host, words, false), log("scaled request refused");
            else if (!SmokeHook(host, true))
                smoke_phase = 0, colour_held = -1, SmokeScaled(host, false), StepPatch(host, words, false), log("hook refused");
            else if (!StepPatch(host, SmokeSites, true))
                colour_held = -1, unhook("idle loops refused");
            else
                smoke_phase = 2, smoke_on = smoke_tick, smoke_known = *effects, log("hooked");
        } else if (smoke_phase == 2) {
            const auto effects = SmokeEffects(host);
            const bool puffed = effects && std::ranges::any_of(*effects, [&](u64 n) { return std::ranges::find(smoke_known, n) == smoke_known.end(); });
            if (puffed)
                unhook("puffed");
            else if (!play_on || !effects || !SmokeReady(host) || smoke_tick - smoke_on > SmokeWait)
                colour_held = -1, unhook("given up");
        } else if (smoke_phase == 3 && smoke_tick - smoke_on >= 30) {
            smoke_phase = 0;
            const bool scaled = SmokeScaled(host, false), code = StepPatch(host, words, false);
            if (!scaled || !code)
                log("RESTORE FAILED");
        }
        if (colour_held >= 0 && (smoke_phase == 0 || (smoke_phase == 3 && smoke_tick - smoke_on >= SmokeCover)))
            colour_held = -1;
    }

    // Kirby's spray on the Arenas' figure and plate, in their keys (nothing for the game's pink): the figure's colours
    // (KirbyColour::FigureColours), and the plate's skin and name ink
    const KirbyColour::Preset& ColourShown() const {
        return colour_test ? *colour_test : KirbyColour::Presets[static_cast<std::size_t>(colour_want)];
    }
    std::string FigureToken() const {
        const auto& p = ColourShown();
        if (p.path == KirbyColour::Path::Pink)
            return {};
        std::string token = ".";
        for (const std::uint32_t c : KirbyColour::FigureColours(p)) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "%06X", c & 0xFFFFFF);
            token += hex;
        }
        return token;
    }
    std::string PlateToken() const {
        const auto& p = ColourShown();
        if (p.path == KirbyColour::Path::Pink)
            return {};
        char hex[16];
        std::snprintf(hex, sizeof hex, "/%02X%02X%02X%02X%02X%02X", p.skin.r, p.skin.g, p.skin.b, p.label.r, p.label.g, p.label.b);
        return hex;
    }

    int ColourStored(int file) const {
        int v = 0;
        for (int b = 0; b < ColourBits; ++b)
            v |= (options[static_cast<std::size_t>(ColourBit + ColourBits * file + b)] ? 1 : 0) << b;
        return v < static_cast<int>(KirbyColour::Presets.size()) && !KirbyColour::Presets[static_cast<std::size_t>(v)].key.empty() ? v : 0;
    }

    bool ModelAt(const EdenDsmodHostApi& host, u64 base) const {
        if (!base)
            return false;
        const u8* head = host.get_read_pointer(host.userdata, base, 4);
        const u8* mark = head ? host.get_read_pointer(host.userdata, base + KirbyColour::ModelMark, 4 * KirbyColour::ModelMarkWords.size()) : nullptr;
        return mark && std::memcmp(head, "FRES", 4) == 0 &&
               std::memcmp(mark, KirbyColour::ModelMarkWords.data(), 4 * KirbyColour::ModelMarkWords.size()) == 0;
    }

    void ColourModelStep(const EdenDsmodHostApi& host) {
        if (!options_known || !host.write_memory)
            return;
        // each file has its own colour (the game's pink until a file is chosen)
        if (files_cursor <= 2 && static_cast<int>(files_cursor) != colour_file)
            colour_file = static_cast<int>(files_cursor), colour_want = ColourStored(colour_file);
        colour_known = true;
        // the flags follow the colour wanted: set (not toggled, a rule may run twice in a tick) until their values come back
        colour_storing = colour_file >= 0 && ColourStored(colour_file) != colour_want;
        if (colour_test_poll++ % 60 == 0)
            ColourTestPoll();
        // (a colour on trial takes the file's place: written as preset 1000 + its version)
        const int target = colour_test ? 1000 + colour_test_version : colour_held >= 0 ? colour_held : colour_want;
        // the game's own pink needs nothing until another colour was put on this boot
        if (target == 0 && colour_written < 0)
            return;
        if (!ModelAt(host, colour_base)) {
            colour_base = 0;
            // (the world map's Kirby is this model too, already there before any stage)
            if (!play_on && !wmap_live)
                return;
            // the hero's model points at its resource's FMDL (not Kirby's when the hero is another character, in the
            // Arenas); without one the heap is searched, a slice a tick
            if (const u64 fmdl = KirbyModelField(host, 0x80); fmdl > 0xF0) {
                if (fmdl - 0xF0 == colour_refused || !ModelAt(host, fmdl - 0xF0))
                    return;
                colour_base = fmdl - 0xF0;
            }
            const u64 begin = host.get_heap_begin(host.userdata) & ~u64{0xFFF}, end = host.get_heap_end(host.userdata);
            if (colour_cursor < begin || colour_cursor >= end)
                colour_cursor = begin;
            for (int n = 0; n < 16384 && colour_cursor < end && !colour_base; ++n, colour_cursor += 0x1000)
                if (colour_cursor != colour_refused && ModelAt(host, colour_cursor))
                    colour_base = colour_cursor;
            if (!colour_base)
                return;
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod colour: Kirby's model at " + KirbyHex(colour_base)).c_str());
        } else if (++colour_tick % 30 != 0 && colour_written == target)
            return;
        ColourWrite(host, target);
    }

    void ColourWrite(const EdenDsmodHostApi& host, int target) {
        const auto log = [&](const std::string& m) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod colour: " + m).c_str());
        };
        if (!colour_model_read) {
            colour_model_read = true;
            if (const auto packed = romfs(std::string{KirbyColour::ModelPath}))
                if (auto model = KirbyAssets::DecompressCmp(*packed))
                    colour_model = std::move(*model);
            colour_writes.resize(KirbyColour::Presets.size());
            if (colour_model.empty())
                log("the model's file could not be read");
        }
        if (colour_model.empty())
            return;
        const KirbyColour::Preset& preset = target >= 1000 ? *colour_test : KirbyColour::Presets[static_cast<std::size_t>(target)];
        auto& writes = target >= 1000 ? colour_test_writes : colour_writes[static_cast<std::size_t>(target)];
        if (writes.empty())
            writes = KirbyColour::PresetWrites(colour_model, preset);
        if (writes.empty())
            return;
        // all checked before any is written: a code word must still be the model's instruction (its constant or ours)
        int changes = 0;
        for (const auto& w : writes) {
            const u8* now = host.get_read_pointer(host.userdata, colour_base + w.at, w.size);
            if (!now)
                return;
            if (std::memcmp(now, w.bytes.data(), w.size) == 0)
                continue;
            if (w.code) {
                u64 word, own;
                std::memcpy(&word, now, 8), std::memcpy(&own, colour_model.data() + w.at, 8);
                if (!KirbyColour::SameInstruction(own, word)) {
                    log("unexpected word at +" + KirbyHex(w.at) + ", nothing written");
                    colour_refused = colour_base, colour_base = 0;
                    return;
                }
            }
            ++changes;
        }
        for (const auto& w : writes)
            if (const u8* now = host.get_read_pointer(host.userdata, colour_base + w.at, w.size); now && std::memcmp(now, w.bytes.data(), w.size) != 0)
                host.write_memory(host.userdata, colour_base + w.at, w.bytes.data(), w.size);
        if (changes) {
            log(std::string{preset.key} + " put on (" + std::to_string(changes) + " writes)");
            OutlineNow(host, writes);
        }
        colour_written = target;
        if (preset.rainbow)
            OutlineRainbow(host);
    }

    // A field of the hero's model wrapper ([[P+0x130]+0x18], P the hero), 0 when there is none.
    u64 KirbyModelField(const EdenDsmodHostApi& host, u64 off) const {
        u64 root{}, rec{}, p{}, anim{}, model{}, v{};
        const auto rd = [&](u64 at, u64& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, 8); };
        return rd(host.main_base + 0x16325B8, root) && rd(root + 0x260, rec) && rd(rec + 0x140, p) && rd(p + 0x130, anim) &&
                       rd(anim + 0x18, model) && model && rd(model + off, v)
                   ? v
                   : 0;
    }

    // The outline at once: the new defaults reach only the next room's material, so the current one (the model's
    // material objects from [model+0x150], 0x80 each in the materials' order: KbBodyC, KbMouthDrawC, OutlineC) gets
    // them in its source params ([obj+0x48]) and its two dirty words ([obj+0x10], [obj+0x18]) bits 6, 8 and 14.
    void OutlineNow(const EdenDsmodHostApi& host, const std::vector<KirbyColour::ModelWrite>& writes) {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const u64 materials = KirbyModelField(host, 0x150), obj = materials ? materials + 2 * 0x80 : 0;
        u64 fmat{}, src{}, dirty[2]{};
        if (!obj || !rd(obj, fmat) || fmat != colour_base + 0x8A0 || !rd(obj + 0x48, src) || !src || !rd(obj + 0x10, dirty[0]) ||
            !rd(obj + 0x18, dirty[1]))
            return;
        for (const auto& w : writes)
            if (!w.code && w.at >= KirbyColour::OutlineDefaults && w.at < KirbyColour::OutlineDefaults + 0x48)
                host.write_memory(host.userdata, src + (w.at - KirbyColour::OutlineDefaults), w.bytes.data(), w.size);
        for (const u64 d : dirty)
            if (u32 v{}; d && rd(d, v))
                v |= 0x4140, host.write_memory(host.userdata, d, &v, 4);
    }

    // The rainbow kept on the current outline (a candy's end turns InvincibleNoKey off there; bit 6 its dirty bit)
    void OutlineRainbow(const EdenDsmodHostApi& host) {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const u64 materials = KirbyModelField(host, 0x150), obj = materials ? materials + 2 * 0x80 : 0;
        u64 fmat{}, src{}, dirty[2]{};
        float on{};
        if (!obj || !rd(obj, fmat) || fmat != colour_base + 0x8A0 || !rd(obj + 0x48, src) || !src || !rd(obj + 0x10, dirty[0]) ||
            !rd(obj + 0x18, dirty[1]) || !rd(src + 0x18, on) || on != 0)
            return;
        on = 1;
        host.write_memory(host.userdata, src + 0x18, &on, 4);
        for (const u64 d : dirty)
            if (u32 v{}; d && rd(d, v))
                v |= 0x40, host.write_memory(host.userdata, d, &v, 4);
    }

    // Magoland's Kirby (gfx/Yy/.../Kirby/...Model: the park's and most attractions', Samurai Kirby's, the shuriken game's)
    // has his colours as params of his materials, BodyC and OutlineC (KirbyColour::YyColoursOf). They go in each such model
    // the game holds (a page starting with FRES whose BodyC samples KirbyEyeMask first), which the Kirbys made after copy,
    // and in the Kirbys already there: their MaterialObjs, found by a pointer to the material, take them in their src
    // params ([+0x48]) with each param's bit in both dirty words ([+0x10], [+0x18], 4 bytes apart), and in the two
    // buffers the GPU reads them from, which the game fills from the src params only now and then (OutlineC's not while a
    // scene goes on, so a rainbow put on there flashed): at [+0x38] in the memory pool [[+0x30]+8], [+0x60] bytes each
    // and 0x100-aligned, the params laid out by size (vec4, vec3, vec2, scalars, each kind in the material's order) with
    // std140's alignment.
    struct YyParam {
        u32 at{}, bit{}, ubo{};
    };
    struct YyMaterial {
        u64 fmat{}, data{};
        u32 index{}, ubo_size{};
        YyParam textuur{};               // BodyC's TextuurSwitchNoKey: (0, 0) for player 1's colours
        std::array<YyParam, 6> colour{}; // cheek, foot, body lit, then in shadow (BodyC's)
        YyParam rainbow{};               // OutlineC's InvincibleNoKey
    };
    struct YyModel {
        u64 base{};
        std::array<YyMaterial, 2> mat{}; // BodyC, OutlineC (not in every model)
    };
    std::vector<YyModel> yy_models;
    std::set<u64> yy_skip; // other FRES pages, until the scan comes round again
    std::map<u64, u64> yy_unready; // FRES pages not yet relocated (the tick first seen): looked at every tick for a while
    std::vector<std::pair<u64, int>> yy_objs; // a Kirby's MaterialObj, its material (model * 2 + BodyC / OutlineC)
    // where models and Kirbys were found before (16 MB around each), searched first: the game loads an attraction's model
    // again where it was, and a Kirby is made next to the last ones
    std::vector<u64> yy_hot_models, yy_hot_objs;
    std::vector<u64> yy_recent; // the last Kirbys found: a new one (an attraction's results, its intro) is made just past them
    std::vector<std::pair<u64, u64>> yy_ptr_ranges; // a pass over the heap: the hot places first
    u64 yy_cursor{}, yy_hot_cursor{}, yy_watch{}, yy_tick{};
    bool yy_ptr_pass{};
    int yy_written{-1};

    std::string GuestString(const EdenDsmodHostApi& host, u64 at) const {
        u16 n{};
        if (!at || !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &n, 2) || n > 64)
            return {};
        std::string s(n, '\0');
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at + 2, s.data(), n) ? s : std::string{};
    }

    std::optional<YyModel> YyModelAt(const EdenDsmodHostApi& host, u64 base) const {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        u64 magic{}, model{}, mats{};
        u16 count{};
        if (!rd(base, magic) || magic != 0x2020202053455246 || !rd(base + 0x28, model) || !rd(model + 0x38, mats) || !rd(model + 0x6C, count) ||
            count > 16)
            return std::nullopt;
        YyModel m{base};
        for (u16 i = 0; i < count; ++i) {
            const u64 fmat = mats + u64{i} * 0xA8;
            u64 name{}, dict{}, info{}, data{}, texs{}, tex{};
            u32 n{};
            if (!rd(fmat + 8, name) || !rd(fmat + 0x58, dict) || !rd(fmat + 0x50, info) || !rd(fmat + 0x60, data))
                return std::nullopt;
            const std::string material = GuestString(host, name);
            const int which = material == "BodyC" ? 0 : material == "OutlineC" ? 1 : -1;
            if (which < 0)
                continue;
            if (which == 0 && (!rd(fmat + 0x30, texs) || !rd(texs, tex) || !GuestString(host, tex).starts_with("KirbyEyeMask")))
                return std::nullopt;
            if (!rd(dict + 4, n) || n > 64)
                return std::nullopt;
            YyMaterial mt{fmat, data, i};
            // the GPU's layout
            std::vector<std::pair<u32, u8>> sizes; // index, bytes
            for (u32 k = 0; k < n; ++k) {
                u8 size{};
                if (!rd(info + k * 0x20 + 0x11, size))
                    return std::nullopt;
                sizes.emplace_back(k, size);
            }
            std::ranges::stable_sort(sizes, [](const auto& x, const auto& y) { return x.second > y.second; });
            std::vector<u32> ubo(n);
            u32 end = 0;
            for (const auto& [k, size] : sizes) {
                const u32 align = size > 8 ? 16 : std::max<u32>(size, 1);
                end = (end + align - 1) / align * align, ubo[k] = end, end += size;
            }
            mt.ubo_size = (end + 15) / 16 * 16;
            int colours = 0;
            for (u32 k = 0; k < n; ++k) {
                u64 pname{};
                u16 off{};
                if (!rd(dict + 8 + 0x10 * (k + 1) + 8, pname) || !rd(info + k * 0x20 + 0x12, off))
                    return std::nullopt;
                const std::string param = GuestString(host, pname);
                const YyParam at{off, k < 32 ? 1u << k : 0u, ubo[k]};
                if (param == "CheekColor_NoKey")
                    mt.colour[0] = at, ++colours;
                else if (param == "FootColor_NoKey")
                    mt.colour[1] = at, ++colours;
                else if (param == "BodyColor_NoKey")
                    mt.colour[2] = at, ++colours;
                else if (which == 0 && param == "CheekColor_NoKey8")
                    mt.colour[3] = at;
                else if (which == 0 && param == "FootColor_NoKey7")
                    mt.colour[4] = at;
                else if (which == 0 && param == "BodyColor_NoKey7")
                    mt.colour[5] = at;
                else if (param == "InvincibleNoKey")
                    mt.rainbow = at;
                else if (which == 0 && param == "TextuurSwitchNoKey")
                    mt.textuur = at;
            }
            // (the attractions' own OutlineC has the rainbow only)
            if (colours == 3 || (which == 1 && mt.rainbow.bit))
                m.mat[static_cast<std::size_t>(which)] = mt;
        }
        return m.mat[0].fmat ? std::optional{m} : std::nullopt;
    }

    // A material's colours and rainbow at `params` (its defaults or a Kirby's src params, or with `ubo` one of its GPU
    // buffers): the bits of those that changed
    u32 YyWrite(const EdenDsmodHostApi& host, const YyMaterial& mt, u64 params, const KirbyColour::YyColours& c, float rainbow,
                bool ubo = false) {
        u32 bits = 0;
        const auto put = [&](const YyParam& p, const void* v, u32 size) {
            const u64 at = params + (ubo ? p.ubo : p.at);
            const u8* now = host.get_read_pointer(host.userdata, at, size);
            if (now && std::memcmp(now, v, size) != 0)
                host.write_memory(host.userdata, at, v, size), bits |= p.bit;
        };
        for (std::size_t i = 0; i < mt.colour.size(); ++i)
            if (mt.colour[i].bit)
                put(mt.colour[i], c[i].data(), 12);
        if (mt.rainbow.bit)
            put(mt.rainbow, &rainbow, 4);
        return bits;
    }

    // Research: user:yytrace.txt holding 1 logs what Magoland's Kirby does, a line per change ("Kirby DSMod yytrace t=<tick>
    // ..."): the FRES pages of the places searched first (1 read, 2 relocated, 3 Kirby's, and then the Kirbys made already,
    // by a pass over the heap), the defaults and Kirbys written, and each Kirby's body red or rainbow in both its param blocks
    // ([+0x48], [+0x50]) with its dirty words. 2 also puts the rainbow on the Kirbys there in the first block, 3 in both.
    int yy_trace{};
    int yy_trace_done{};
    std::map<u64, int> yy_trace_pages;
    std::map<u64, std::string> yy_trace_objs;

    void YyTraceLog(const EdenDsmodHostApi& host, const std::string& m) const {
        if (yy_trace && host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod yytrace t=" + std::to_string(yy_tick) + " " + m).c_str());
    }

    void YyTrace(const EdenDsmodHostApi& host, const std::vector<u64>& windows, u64 begin, u64 end) {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        if (yy_tick % 60 == 1) {
            const auto t = romfs("user:yytrace.txt");
            yy_trace = t && !t->empty() && (*t)[0] >= '1' && (*t)[0] <= '3' ? (*t)[0] - '0' : 0;
        }
        if (!yy_trace)
            return;
        for (const u64 from : windows)
            for (u64 page = from; page < from + 0x1000000; page += 0x1000) {
                const u8* head = host.get_read_pointer(host.userdata, page, 8);
                int state = 0;
                if (head && std::memcmp(head, "FRES    ", 8) == 0) {
                    u64 model{};
                    state = rd(page + 0x28, model) && model >= page && model < page + 0x4000000 ? 2 : 1;
                    if (state == 2 && std::ranges::any_of(yy_models, [&](const YyModel& m) { return m.base == page; }))
                        state = 3;
                }
                const auto it = yy_trace_pages.find(page);
                const int was = it == yy_trace_pages.end() ? 0 : it->second;
                if (state == was)
                    continue;
                if (state)
                    yy_trace_pages[page] = state;
                else
                    yy_trace_pages.erase(page);
                YyTraceLog(host, "page " + KirbyHex(page) + " " + std::to_string(was) + "->" + std::to_string(state));
                if (state != 3)
                    continue;
                const auto m = std::ranges::find_if(yy_models, [&](const YyModel& y) { return y.base == page; });
                std::string found;
                for (u64 p = begin; p < end; p += 0x1000) {
                    const u8* q = host.get_read_pointer(host.userdata, p, 0x1000);
                    for (u32 at = 0; q && at < 0x1000; at += 8) {
                        u64 v;
                        std::memcpy(&v, q + at, 8);
                        if (v && (v == m->mat[0].fmat || v == m->mat[1].fmat))
                            found += " " + KirbyHex(p + at) + (v == m->mat[0].fmat ? "/body" : "/outline");
                    }
                }
                YyTraceLog(host, "at parse, made already:" + (found.empty() ? std::string{" none"} : found));
            }
        for (const auto& [obj, id] : yy_objs) {
            const YyMaterial& mt = yy_models[static_cast<std::size_t>(id / 2)].mat[static_cast<std::size_t>(id % 2)];
            u64 a{}, b{}, d0{}, d1{};
            u32 w0{}, w1{};
            float va{}, vb{};
            const YyParam p = id % 2 ? mt.rainbow : mt.colour[2];
            if (!rd(obj + 0x48, a) || !rd(obj + 0x50, b) || !rd(obj + 0x10, d0) || !rd(obj + 0x18, d1))
                continue;
            rd(d0, w0), rd(d1, w1), rd(a + p.at, va), rd(b + p.at, vb);
            // the experiment: the rainbow put on in one block or both
            if (id % 2 && yy_trace >= 2 && yy_trace_done != yy_trace && p.bit) {
                const float on = 1;
                host.write_memory(host.userdata, a + p.at, &on, 4);
                if (yy_trace == 3)
                    host.write_memory(host.userdata, b + p.at, &on, 4);
                for (const u64 d : {d0, d1})
                    if (u32 v{}; rd(d, v))
                        v |= p.bit, host.write_memory(host.userdata, d, &v, 4);
                YyTraceLog(host, "rainbow put on " + KirbyHex(obj) + (yy_trace == 3 ? " in both blocks" : " in the first block"));
            }
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s A=%g B=%g dirty=%08x %08x", id % 2 ? "rainbow" : "body red", va, vb, w0, w1);
            if (yy_trace_objs[obj] != buf)
                yy_trace_objs[obj] = buf, YyTraceLog(host, "Kirby " + KirbyHex(obj) + " " + buf);
        }
        if (!yy_objs.empty())
            yy_trace_done = yy_trace;
    }

    void YyColourStep(const EdenDsmodHostApi& host) {
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        const auto log = [&](const std::string& m) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod colour: " + m).c_str());
        };
        if (!colour_known || colour_file < 0 || play_on || wmap_live || !host.write_memory) {
            yy_ptr_pass = false;
            return;
        }
        const int target = colour_test ? 1000 + colour_test_version : colour_want;
        const KirbyColour::Preset& preset = target >= 1000 ? *colour_test : KirbyColour::Presets[static_cast<std::size_t>(target)];
        const auto colours = KirbyColour::YyColoursOf(preset);
        const u64 begin = host.get_heap_begin(host.userdata) & ~u64{0xFFF}, end = host.get_heap_end(host.userdata);
        ++yy_tick;
        constexpr u64 Hot = 0x1000000;
        const auto hot = [&](std::vector<u64>& list, u64 at) {
            if (std::ranges::none_of(list, [&](u64 from) { return at >= from && at < from + Hot; }))
                list.push_back((at - Hot / 2) & ~u64{0xFFF});
        };
        // the models: those known checked twice a second, the page starts of their places every few ticks and of the heap
        // a slice a tick
        if (yy_tick % 30 == 0 && std::erase_if(yy_models, [&](const YyModel& m) { return !YyModelAt(host, m.base); }))
            yy_objs.clear();
        const auto look = [&](u64 page) {
            const u8* head = host.get_read_pointer(host.userdata, page, 8);
            if (!head || std::memcmp(head, "FRES    ", 8) != 0 || yy_skip.contains(page) ||
                std::ranges::any_of(yy_models, [&](const YyModel& m) { return m.base == page; }))
                return;
            // (a model just read still has offsets for pointers until the game relocates it: looked at again each tick, as
            // Kirby is already made when it is)
            if (u64 model{}; !rd(page + 0x28, model) || model < page || model >= page + 0x4000000) {
                yy_unready.try_emplace(page, yy_tick);
                return;
            }
            yy_unready.erase(page);
            if (auto m = YyModelAt(host, page)) {
                yy_models.push_back(*m), yy_written = -1;
                hot(yy_hot_models, page);
                log("Magoland's Kirby at " + KirbyHex(page));
            } else
                yy_skip.insert(page);
        };
        // (a place a tick; before any is known, where the game put them in every boot seen, from the heap's start: the
        // park's model through the main menu about +0x13000000..+0x16000000, through the world map's X at +0x10CA5000, the
        // attractions' about +0x27000000..+0x29000000; and every Kirby, the park's, the attractions' and the mask stand's, in
        // +0x54000000..+0x5C000000)
        if (yy_hot_models.empty())
            for (const u64 at : {0x10000000, 0x13000000, 0x14000000, 0x15000000, 0x27000000, 0x28000000})
                yy_hot_models.push_back(begin + at);
        if (yy_hot_objs.empty())
            for (u64 at = 0x54000000; at < 0x5C000000; at += Hot)
                yy_hot_objs.push_back(begin + at);
        std::erase_if(yy_unready, [&](const auto& u) { return yy_tick - u.second > 600; });
        for (const auto& [page, since] : std::map{yy_unready})
            look(page);
        {
            const u64 from = yy_hot_models[static_cast<std::size_t>(yy_hot_cursor++ % yy_hot_models.size())];
            for (u64 page = from; page < from + Hot; page += 0x1000)
                look(page);
        }
        if (yy_cursor < begin || yy_cursor >= end)
            yy_cursor = begin, yy_skip.clear();
        for (int n = 0; n < 8192 && yy_cursor < end; ++n, yy_cursor += 0x1000)
            look(yy_cursor);
        YyTrace(host, yy_hot_models, begin, end);
        if (yy_models.empty())
            return;
        const auto material = [&](int id) -> const YyMaterial& { return yy_models[static_cast<std::size_t>(id / 2)].mat[static_cast<std::size_t>(id % 2)]; };
        // player 1's Kirby: his BodyC (the instance's MaterialObjs follow each other, 0x80 apart, in the materials' order)
        // takes the colours of TextuurSwitch (0, 0), the pink's, which hold ours; the others (the CPU's, other players')
        // take theirs
        const auto player_one = [&](u64 obj, int id) {
            const YyMaterial& body = yy_models[static_cast<std::size_t>(id / 2)].mat[0];
            const u64 b = obj - (static_cast<s64>(material(id).index) - static_cast<s64>(body.index)) * 0x80;
            u64 f{}, params{};
            std::array<float, 2> ts{};
            return body.textuur.bit && rd(b, f) && f == body.fmat && rd(b + 0x48, params) && rd(params + body.textuur.at, ts) && ts[0] == 0 &&
                   ts[1] == 0;
        };
        // a Kirby's own: the colours in its src params with their bits marked, and in its GPU buffers when they are where
        // and as big as the material's layout says; the rainbow only on player 1's
        const auto update = [&](u64 obj, int id) {
            const YyMaterial& mt = material(id);
            const auto& c = colours[static_cast<std::size_t>(id % 2)];
            const float rainbow = preset.rainbow && id % 2 && player_one(obj, id) ? 1.0f : 0.0f;
            u64 params{}, dirty[2]{}, pool{}, base{}, at{}, size{};
            if (!rd(obj + 0x48, params) || !params || !rd(obj + 0x10, dirty[0]) || !rd(obj + 0x18, dirty[1]))
                return;
            if (const u32 bits = YyWrite(host, mt, params, c, rainbow)) {
                for (const u64 d : dirty)
                    if (u32 v{}; d && rd(d, v))
                        v |= bits, host.write_memory(host.userdata, d, &v, 4);
                YyTraceLog(host, "Kirby " + KirbyHex(obj) + " written");
            }
            if (rd(obj + 0x30, pool) && rd(pool + 8, base) && base && rd(obj + 0x38, at) && rd(obj + 0x60, size) && size == mt.ubo_size)
                for (u64 k = 0; k < 2; ++k)
                    YyWrite(host, mt, base + at + k * ((size + 0xFF) & ~u64{0xFF}), c, rainbow, true);
        };
        // (every tick: an attraction loaded again where it was brings the game's colours back)
        {
            bool changed = false;
            for (const auto& m : yy_models)
                for (int w = 0; w < 2; ++w)
                    if (m.mat[static_cast<std::size_t>(w)].fmat)
                        changed |= YyWrite(host, m.mat[static_cast<std::size_t>(w)], m.mat[static_cast<std::size_t>(w)].data,
                                           colours[static_cast<std::size_t>(w)], 0) != 0;
            if (changed)
                YyTraceLog(host, "defaults written");
            // the Kirbys made before: those known at once, the others found by a pass over the heap
            if (changed || yy_written != target) {
                for (const auto& [obj, id] : yy_objs)
                    update(obj, id);
                yy_ptr_ranges.clear();
                for (const u64 from : yy_hot_objs)
                    yy_ptr_ranges.emplace_back(from, from + Hot);
                yy_ptr_ranges.emplace_back(begin, end);
                yy_ptr_pass = true;
            }
            yy_written = target;
        }
        if (yy_tick % 30 == 0) {
            std::erase_if(yy_objs, [&](const auto& o) { u64 f{}; return !rd(o.first, f) || f != material(o.second).fmat; });
            // (a Kirby is made with player 1's TextuurSwitch and gets a CPU's a moment later: the rainbow looked at again)
            for (const auto& [obj, id] : yy_objs)
                if (u64 params{}; id % 2 && material(id).rainbow.bit && rd(obj + 0x48, params))
                    if (float on{}; rd(params + material(id).rainbow.at, on) && (on != 0) != (preset.rainbow && player_one(obj, id)))
                        update(obj, id);
        }
        std::vector<std::pair<u64, int>> fmats;
        for (std::size_t i = 0; i < yy_models.size(); ++i)
            for (int w = 0; w < 2; ++w)
                if (const u64 f = yy_models[i].mat[static_cast<std::size_t>(w)].fmat)
                    fmats.emplace_back(f, static_cast<int>(i * 2) + w);
        // the Kirbys on a page: pointers to a material, then the two dirty words side by side
        u64 low = ~u64{0}, high = 0;
        for (const auto& [f, id] : fmats)
            low = std::min(low, f), high = std::max(high, f);
        const auto scan = [&](u64 at_page) {
            const u8* page = host.get_read_pointer(host.userdata, at_page, 0x1000);
            for (u32 at = 0; page && at < 0x1000; at += 8) {
                u64 v;
                std::memcpy(&v, page + at, 8);
                if (v < low || v > high)
                    continue;
                for (const auto& [f, id] : fmats) {
                    if (v != f)
                        continue;
                    const u64 obj = at_page + at;
                    u64 d0{}, d1{}, params{};
                    if (!rd(obj + 0x10, d0) || !rd(obj + 0x18, d1) || d1 != d0 + 4 || !rd(obj + 0x48, params) || !params ||
                        std::ranges::any_of(yy_objs, [&](const auto& o) { return o.first == obj; }))
                        continue;
                    yy_objs.emplace_back(obj, id);
                    hot(yy_hot_objs, obj);
                    std::erase(yy_recent, obj);
                    yy_recent.push_back(obj);
                    if (yy_recent.size() > 4)
                        yy_recent.erase(yy_recent.begin());
                    update(obj, id);
                    std::array<float, 2> ts{-1, -1};
                    if (const YyMaterial& body = yy_models[static_cast<std::size_t>(id / 2)].mat[0]; body.textuur.bit)
                        if (u64 b{}, bp{}; (b = obj - (static_cast<s64>(material(id).index) - static_cast<s64>(body.index)) * 0x80) && rd(b + 0x48, bp))
                            rd(bp + body.textuur.at, ts);
                    char what[96];
                    std::snprintf(what, sizeof what, " (%s, TextuurSwitch %g %g, player 1: %d)", id % 2 ? "outline" : "body", ts[0], ts[1],
                                  player_one(obj, id) ? 1 : 0);
                    log("a Kirby's material at " + KirbyHex(obj) + " (heap +" + KirbyHex(obj - begin) + ")" + what);
                }
            }
        };
        // the heap, after a change; and always the places Kirbys were found, for those made since (a quarter a tick)
        for (int n = 0; n < 2048 && yy_ptr_pass && !yy_ptr_ranges.empty(); ++n) {
            auto& [cursor, stop] = yy_ptr_ranges.front();
            if (cursor >= stop) {
                yy_ptr_ranges.erase(yy_ptr_ranges.begin());
                continue;
            }
            scan(cursor);
            cursor += 0x1000;
        }
        if (yy_ptr_ranges.empty())
            yy_ptr_pass = false;
        // (twice as fast while the eyes are ours: a Kirby not yet written has black ones, his buffers holding NaN there)
        const bool eyes = KirbyColour::HasEyes(preset);
        for (int n = 0; n < (eyes ? 2 : 1) && !yy_hot_objs.empty(); ++n) {
            const u64 from = yy_hot_objs[static_cast<std::size_t>(yy_watch / 4 % yy_hot_objs.size())] + yy_watch % 4 * (Hot / 4);
            for (u64 page = from; page < from + Hot / 4; page += 0x1000)
                scan(page);
            ++yy_watch;
        }
        // and just past the last ones found every tick
        if (eyes) {
            std::vector<std::pair<u64, u64>> near;
            for (const u64 at : yy_recent)
                near.emplace_back((at - 0x10000) & ~u64{0xFFF}, (at + 0xC0000) & ~u64{0xFFF});
            std::ranges::sort(near);
            u64 done = 0;
            for (const auto& [from, to] : near)
                for (u64 page = std::max(from, done); page < to; page += 0x1000)
                    scan(page), done = page + 0x1000;
        }
        // the eyes (KirbyColour::YyEyePrograms): the body programs take the per-Kirby eye while the colour has eyes of its
        // own, the game's words back otherwise (a program is one of the table's when each word is the game's or ours);
        // every Kirby's BodyC buffers hold his coefficients
        for (const auto& m : yy_models)
            for (const auto& prog : KirbyColour::YyEyePrograms)
                for (const u32 code : prog.code) {
                    const auto word = [&](const KirbyColour::YyEyeWord& w) {
                        u64 v{};
                        const u8* p = code ? host.get_read_pointer(host.userdata, m.base + code + w.at, 8) : nullptr;
                        return p ? (std::memcpy(&v, p, 8), v) : u64{};
                    };
                    if (!std::ranges::all_of(prog.words, [&](const auto& w) { const u64 v = word(w); return v == w.game || v == w.ours; }))
                        continue;
                    for (const auto& w : prog.words)
                        if (const u64 want = eyes ? w.ours : w.game; word(w) != want)
                            host.write_memory(host.userdata, m.base + code + w.at, &want, 8);
                }
        const auto ours = KirbyColour::YyEyeOf(&preset), game = KirbyColour::YyEyeOf(nullptr);
        for (const auto& [obj, id] : yy_objs) {
            u64 pool{}, base{}, at{}, size{};
            if (id % 2 || !rd(obj + 0x30, pool) || !rd(pool + 8, base) || !base || !rd(obj + 0x38, at) || !rd(obj + 0x60, size) ||
                size < KirbyColour::YyEyePad + 0x10 * ours.size())
                continue;
            const auto& k = player_one(obj, id) ? ours : game;
            for (u64 b = 0; b < 2; ++b)
                for (std::size_t i = 0; i < k.size(); ++i) {
                    const u64 to = base + at + b * ((size + 0xFF) & ~u64{0xFF}) + KirbyColour::YyEyePad + 0x10 * i;
                    if (const u8* now = host.get_read_pointer(host.userdata, to, 4); now && std::memcmp(now, &k[i], 4) != 0)
                        host.write_memory(host.userdata, to, &k[i], 4);
                }
        }
    }

    // The colour menu (KirbyModes::Colour*): the ribbon on Kirby's pages opens it, popping in; a slot shows its colour in the
    // name bar, Apply stores it and puts it on Kirby, Back pops it out. Each popup picture is scaled about the popup's
    // centre C (col.pop, its pivot there); a control's own push q about its centre M and the slots' scroll S come as
    // offsets: s (1 - q) (M - C) + s (0, -S), at scale s q.
    static constexpr float ColourLeft = 40, ColourTop = 40, ColourPopTicks = 12, ColourShutTicks = 8;
    struct ColourUi {
        int phase{}; // 0 shut, 1 popping in, 2 open, 3 popping out
        bool opening{};
        u64 at{};
        int sel{};
        float scroll{};
        std::array<u64, 3> pushed{~u64{}, ~u64{}, ~u64{}}; // ribbon, Apply, Back
        int slot{-1};
        u64 slot_at{};
        std::map<std::string, std::string> shown; // per bind, the last picture drawn (published every tick)
    } col;
    std::atomic<int> col_tap{-1}, col_pan{0}, col_thumb{0};

    std::optional<KirbyAssets::Image> RenderColour(const std::vector<std::string>& f) {
        const auto preset = [&](const std::string& i) -> const KirbyColour::Preset& {
            return KirbyColour::Presets[std::min<std::size_t>(std::strtoul(i.c_str(), nullptr, 10), KirbyColour::Presets.size() - 1)];
        };
        const auto src = ModesSources();
        // a cut slot: its picture's rows from the last field on are left out
        const auto cut = [](std::optional<KirbyAssets::Image> im, const std::string& rows) {
            const auto keep = std::strtoul(rows.c_str(), nullptr, 10);
            if (im)
                for (std::size_t y = keep; y < im->height; ++y)
                    for (std::size_t x = 0; x < im->width; ++x)
                        im->rgba[(y * im->width + x) * 4 + 3] = 0;
            return im;
        };
        if (f[0] == "colframe")
            return KirbyModes::ColourFrame(src);
        if (f[0] == "colwhite")
            return KirbyAssets::Image{4, 4, std::vector<u8>(64, 0xFF)};
        if (f[0] == "colthumb")
            return KirbyModes::ColourThumb();
        if (f[0] == "colunknown")
            return f.size() == 2 ? cut(KirbyModes::ColourUnknown(src), f[1]) : KirbyModes::ColourUnknown(src);
        if (f[0] == "colback")
            return KirbyModes::ColourBack(src);
        if (f[0] == "colribbon" && f.size() == 2)
            return KirbyModes::ColourRibbon(src, KirbyColour::Spray(preset(f[1]), 3));
        if (f[0] == "colslot" && (f.size() == 3 || f.size() == 4)) {
            auto slot = KirbyModes::ColourSlot(KirbyColour::Spray(preset(f[1]), 3), f[2] == "1");
            return f.size() == 4 ? cut(std::move(slot), f[3]) : slot;
        }
        if (f[0] == "colapply" && f.size() == 3)
            return KirbyModes::ColourApply(src, KirbyModes::OptionTexts(f[1]).apply, f[2] == "1");
        if (f[0] == "colheader" && f.size() == 3) {
            const auto& p = preset(f[2]);
            const auto icon = src.texture("lyt/StaffCredit/PartsLang/Lang/PositionIcon05^u");
            const auto kirby = icon ? KirbyColour::FilmKirby(*icon, p) : std::nullopt;
            const std::u32string name{KirbyColour::NameIn(p, f[1], romfs)};
            const auto rgb = [](KirbyColour::Srgb c) { return KirbyPaint::Rgb{static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b)}; };
            return KirbyModes::ColourHeader(src, name, rgb(p.label), rgb(p.shoes), kirby ? &*kirby : nullptr);
        }
        return std::nullopt;
    }

    void ColourMenuStep(const EdenDsmodHostApi& host) {
        using namespace KirbyModes;
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        // a picture takes its place once drawn; until then the last one stays
        const auto show = [&](const std::string& name, const std::string& key) {
            auto& was = col.shown[name];
            if (key != was && FilesReady(key, true))
                was = key;
            if (!was.empty())
                host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + was).c_str());
        };
        const auto push = [&](u64 at) {
            // ModeSelectBtn*_Push: down to 0.92 and back in 8 frames
            const float q = static_cast<float>(play_tick - at);
            return play_tick < at || q >= 8 ? 1.0f : q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
        };
        const int n = static_cast<int>(KirbyColour::Presets.size()), slots = static_cast<int>(KirbyColour::Slots);
        // only Kirby (not the Arenas' other characters) and only with a file's colour; the menu goes with the page's lock
        // (a load, a menu, Kirby's own scene), as with a room change
        const bool kirby = !arena_mode || arena_character == "Kirby";
        pub("col.kirby", kirby && colour_file >= 0 ? 1 : 0);
        const bool page = ((play_on && play_free) || (mago_on && mago_free)) && kirby && colour_file >= 0 && colour_known &&
                          !title_dir.empty() && host.publish_text;
        const std::string dir = title_dir, sel_key = std::to_string(col.sel);
        const std::string frame = "colframe", white = "colwhite", thumb = "colthumb", back = "colback", unknown = "colunknown";
        const auto header = "colheader/" + dir + "/" + sel_key, apply = "colapply/" + dir + "/" + (col.sel == colour_want ? "1" : "0");
        const auto known = [&](int i) { return i < n && !KirbyColour::Presets[static_cast<std::size_t>(i)].key.empty() && SprayUnlocked(i); };
        const auto whole = [&](int i) { return known(i) ? "colslot/" + std::to_string(i) + "/" + (i == col.sel ? "1" : "0") : std::string{"colunknown"}; };
        const float tall = static_cast<float>(ColourSlotSize[1] + 2 * ColourSlotRing);
        const auto picture_top = [&](int i) {
            return ColourFirst[1] + static_cast<float>(i / ColourColumns) * ColourPitch[1] - col.scroll - static_cast<float>(ColourSlotRing);
        };
        // cut where it would reach past the popup's bottom (KirbyModes::ColourCutStep)
        const auto slot = [&](int i) {
            const float room = ColourCutLimit - picture_top(i);
            if (room >= tall)
                return whole(i);
            return whole(i) + "/" + std::to_string(static_cast<int>(std::max(1.0f, std::floor(room / ColourCutStep)) * ColourCutStep));
        };
        const float rows = static_cast<float>((slots + ColourColumns - 1) / ColourColumns);
        const float view = ColourWindow[3] - ColourWindow[1], top = ColourFirst[1] - ColourWindow[1];
        const float most = std::max(0.0f, top + rows * ColourPitch[1] - (ColourPitch[1] - static_cast<float>(ColourSlotSize[1])) + top - view);
        const float travel = ColourTrack[3] - ColourTrack[1] - static_cast<float>(ColourThumbSize[1]);
        int tap = col_tap.exchange(-1);
        const int pan = col_pan.exchange(0), drag = col_thumb.exchange(0);
        // the current colour's ribbon, shown (under the lock) also while the menu cannot open
        if (colour_known && host.publish_text)
            show("col.ribbon", "colribbon/" + std::to_string(colour_want));
        if (!page) {
            col.phase = 0, col.opening = false;
            pub("col.on", 0), pub("col.live", 0), pub("col.can_open", 0);
            return;
        }
        // the popup's own pictures ahead
        for (const auto& k : {frame, white, thumb, back, unknown})
            FilesAhead(k);
        if (tap == 0 && col.phase == 0) {
            col.pushed[0] = play_tick, col.opening = true, col.sel = colour_want;
            // the slot of the colour Kirby has, in view
            const float row = static_cast<float>(col.sel / ColourColumns) * ColourPitch[1];
            col.scroll = std::clamp(row + top + static_cast<float>(ColourSlotSize[1]) + top - view, 0.0f, most);
        } else if (col.phase == 2 && tap == 1 && col.sel != colour_want)
            col.pushed[1] = play_tick, colour_held = Smoke(host) ? (colour_held >= 0 ? colour_held : colour_want) : -1, colour_want = col.sel;
        else if (col.phase == 2 && tap == 2)
            col.pushed[2] = play_tick, col.phase = 3, col.at = play_tick;
        else if (col.phase == 2 && tap >= 10 && known(tap - 10))
            col.sel = tap - 10, col.slot = tap - 10, col.slot_at = play_tick;
        // opened once its pictures are drawn
        if (col.opening) {
            bool ready = true;
            for (const auto& k : {frame, white, thumb, back, header, apply})
                ready = FilesReady(k, true) && ready;
            for (int i = 0; i < slots; ++i)
                ready = FilesReady(slot(i), true) && ready;
            if (ready || play_tick - col.pushed[0] > 120)
                col.opening = false, col.phase = 1, col.at = play_tick;
        }
        if (col.phase == 2) {
            col.scroll = std::clamp(col.scroll - static_cast<float>(pan) + (travel > 0 ? static_cast<float>(drag) * most / travel : 0), 0.0f, most);
            // the cuts of the slots near the window's bottom, for the scroll
            for (int i = 0; i < slots; ++i)
                if (const float y = picture_top(i); y > ColourWindow[3] - tall - ColourPitch[1] && y < ColourWindow[3] + ColourPitch[1])
                    for (float k = ColourCutStep; k < tall; k += ColourCutStep)
                        FilesAhead(whole(i) + "/" + std::to_string(static_cast<int>(k)));
            // the other colours' pictures, for a quick pick
            for (int i = 0; i < n; ++i) {
                if (!known(i))
                    continue;
                FilesAhead("colheader/" + dir + "/" + std::to_string(i));
                FilesAhead("colslot/" + std::to_string(i) + "/1");
            }
            FilesAhead("colapply/" + dir + "/0"), FilesAhead("colapply/" + dir + "/1");
        }
        float s = 1;
        const float t = static_cast<float>(play_tick - col.at);
        if (col.phase == 1) {
            // easing out with an overshoot
            const float u = std::min(1.0f, t / ColourPopTicks) - 1;
            s = 1 + 2.70158f * u * u * u + 1.70158f * u * u;
            if (t >= ColourPopTicks)
                col.phase = 2, s = 1;
        } else if (col.phase == 3) {
            const float u = std::min(1.0f, t / ColourShutTicks);
            s = 1 - u * u * (2.7f * u - 1.7f);
            if (t >= ColourShutTicks)
                col.phase = 0;
        } else if (col.phase == 0)
            s = 0;
        pub("col.on", col.phase > 0 ? 1 : 0);
        pub("col.live", col.phase == 2 ? 1 : 0);
        pub("col.can_open", col.phase == 0 && !col.opening ? 1 : 0);
        pub("col.can_apply", col.phase == 2 && col.sel != colour_want ? 1 : 0);
        pub("col.pop", std::lround(s * 1000));
        pub("col.scale.ribbon", std::lround(push(col.pushed[0]) * 1000));
        pub("col.scroll", -std::lround(col.scroll));
        if (col.phase == 0)
            return;
        // what is drawn already; the popup's pictures only change once their next one is
        show("col.header", header);
        show("col.apply", apply);
        const float cx = ColourLeft + static_cast<float>(ColourPopupSize[0]) / 2, cy = ColourTop + static_cast<float>(ColourPopupSize[1]) / 2;
        const auto offsets = [&](const std::string& name, float mx, float my, float q, float dy) {
            pub("col." + name + "s", std::lround(s * q * 1000));
            pub("col." + name + "x", std::lround(s * (1 - q) * (mx - cx)));
            pub("col." + name + "y", std::lround(s * ((1 - q) * (my - cy) + dy)));
        };
        for (int i = 0; i < slots; ++i) {
            const std::string k = std::to_string(i);
            show("col.slot." + k, slot(i));
            const float x = ColourFirst[0] + static_cast<float>(i % ColourColumns) * ColourPitch[0];
            const float y = ColourFirst[1] + static_cast<float>(i / ColourColumns) * ColourPitch[1] - col.scroll;
            const float h = static_cast<float>(ColourSlotSize[1]);
            pub("col.sv." + k, y < ColourWindow[3] && y + h > ColourWindow[1] ? 1 : 0);
            offsets("s" + k + ".", ColourLeft + x + static_cast<float>(ColourSlotSize[0]) / 2, ColourTop + y + col.scroll + h / 2,
                    i == col.slot ? push(col.slot_at) : 1, -col.scroll);
        }
        offsets("a.", ColourLeft + (ColourApplyBox[0] + ColourApplyBox[2]) / 2, ColourTop + (ColourApplyBox[1] + ColourApplyBox[3]) / 2,
                push(col.pushed[1]), 0);
        offsets("b.", ColourLeft + ColourBackCircle[0], ColourTop + ColourBackCircle[1], push(col.pushed[2]), 0);
        pub("col.ty", std::lround(s * (most > 0 ? travel * col.scroll / most : 0)));
    }

    // Kirby's colours unlocked by the file's energy spheres (user 2026-11-01): colour_unlocks.txt, under [Story] and [Extra],
    // lines "<spheres>: <colour>, <colour>" with the colours' numbers in the menu (1..86); a colour no line names, or one at
    // 0 spheres, is there from the start. The menu shows the locked ones as "?"; back on the world map a popup comes for each
    // goal the file has reached since the module last saw its spheres (per file, this session)
    struct SprayRule {
        int mode{-1}, spheres{};
    };
    std::array<SprayRule, KirbyColour::Presets.size()> spray_rules{};
    bool spray_read{}, spray_on{};
    int spray_file{-1}, spray_hub{};
    std::array<int, 2> spray_have{-1, -1};                                  // the file's spheres, story and extra
    std::array<std::array<int, 2>, 3> spray_base{{{-1, -1}, {-1, -1}, {-1, -1}}}; // per file, the spheres the popups caught up with
    struct SprayGoal {
        int spheres{};
        std::vector<int> colours;
    };
    std::deque<SprayGoal> spray_queue; // the goals still to show
    std::string spray_shown;
    u64 spray_close{}, spray_pushed{~u64{0} >> 1};
    std::atomic<bool> spray_tap{false};

    bool SprayUnlocked(int i) const {
        const auto& r = spray_rules[static_cast<std::size_t>(i)];
        return r.mode < 0 || i == colour_want || spray_have[static_cast<std::size_t>(r.mode)] >= r.spheres;
    }

    std::string SprayKey(const SprayGoal& goal) const {
        std::string key = "spraypop/" + title_dir + "/" + std::to_string(goal.spheres) + "/";
        for (std::size_t k = 0; k < goal.colours.size(); ++k)
            key += (k ? "." : "") + std::to_string(goal.colours[k]);
        return key;
    }

    void SprayRead() {
        spray_read = true;
        const auto text = romfs("file:colour_unlocks.txt");
        if (!text)
            return;
        const std::string all(text->begin(), text->end());
        int mode = -1;
        for (std::size_t a = 0, e; a < all.size(); a = e + 1) {
            e = std::min(all.find('\n', a), all.size());
            const std::string_view line{all.data() + a, e - a};
            const std::size_t colon = line.find(':');
            if (line.starts_with("[Story]") || line.starts_with("[Extra]"))
                mode = line.starts_with("[Story]") ? 0 : 1;
            else if (mode >= 0 && colon != std::string_view::npos)
                for (std::size_t b = colon + 1, c; b < line.size(); b = c + 1) {
                    c = std::min(line.find(',', b), line.size());
                    const int spheres = std::atoi(std::string{line.substr(0, colon)}.c_str()), n = std::atoi(std::string{line.substr(b, c - b)}.c_str());
                    if (n >= 1 && n <= static_cast<int>(spray_rules.size()) && spheres > 0)
                        spray_rules[static_cast<std::size_t>(n - 1)] = {mode, spheres};
                }
        }
    }

    void SprayStep(const EdenDsmodHostApi& host) {
        if (!spray_read)
            SprayRead();
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        if (colour_file != spray_file)
            spray_file = colour_file, spray_have = {-1, -1}, spray_queue.clear(), spray_on = false;
        // the file's spheres (the stage table says how many each stage has)
        if (colour_file >= 0 && play_tick % 30 == 0) {
            const u64 data = SlotData(host, colour_file, true);
            u32 stages[0x27 * 3]{};
            std::array<std::array<u8, 0x27 * 6>, 2> got{};
            const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
            if (data && rd(host.main_base + 0x132E7B4, stages) && rd(data + 9, got[0]) && rd(data + 0x6C8 + 9, got[1]))
                for (std::size_t m = 0; m < 2; ++m) {
                    int n = 0;
                    for (std::size_t stage = 0; stage < 0x27; ++stage)
                        for (u32 sphere = 0; sphere < std::min<u32>(stages[stage * 3 + 1], 6); ++sphere)
                            n += got[m][stage * 6 + sphere] != 0;
                    spray_have[m] = n;
                }
        }
        // the goals reached since, a popup each, in order
        for (std::size_t m = 0; m < 2 && colour_file >= 0; ++m) {
            int& base = spray_base[static_cast<std::size_t>(colour_file)][m];
            if (spray_have[m] < 0)
                continue;
            if (base >= 0 && spray_have[m] > base) {
                std::map<int, std::vector<int>> goals;
                for (std::size_t i = 0; i < spray_rules.size(); ++i)
                    if (const auto& r = spray_rules[i]; r.mode == static_cast<int>(m) && r.spheres > base && r.spheres <= spray_have[m])
                        goals[r.spheres].push_back(static_cast<int>(i));
                for (auto& [spheres, colours] : goals)
                    spray_queue.push_back({spheres, std::move(colours)});
            }
            base = spray_have[m];
        }
        // (spheres lost again: their goals are not reached any more)
        if (!spray_on)
            std::erase_if(spray_queue, [&](const SprayGoal& goal) { return !SprayUnlocked(goal.colours.front()); });
        // over the world map or a world's hub (the stage page with no stage's spheres up, Kirby free for half a second, the
        // colour menu shut), once drawn; its button's push played, it goes and the next one comes (one left unanswered when
        // the page goes comes back with the next hub)
        spray_hub = play_on && play_hud && !play_stage && play_free && !ship_cover && !arena_mode && col.phase == 0 ? spray_hub + 1 : 0;
        const std::string button = "spraybtn/" + title_dir, key = spray_queue.empty() || title_dir.empty() ? "" : SprayKey(spray_queue.front());
        if (!key.empty())
            FilesAhead(key), FilesAhead(button);
        if (!spray_on && (wmap_live || spray_hub >= 30) && !key.empty() && FilesReady(key, true) && FilesReady(button, true))
            spray_on = true, spray_close = 0;
        if (spray_tap.exchange(false) && spray_on && !spray_close)
            spray_close = spray_pushed = play_tick;
        if (spray_on && spray_close && play_tick - spray_close >= 8)
            spray_on = false, spray_close = 0, spray_queue.pop_front();
        else if (spray_on && !wmap_live && !play_on)
            spray_on = false;
        if (spray_on)
            spray_shown = key;
        pub("spray.popup", spray_on ? 1 : 0);
        float scale = 1;
        if (const float q = static_cast<float>(play_tick - spray_pushed); play_tick >= spray_pushed && q < 8)
            scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
        pub("spray.scale.pop", std::lround(scale * 1000));
        if (!spray_shown.empty() && host.publish_text) {
            host.publish_text(host.userdata, "spray.pop", ("module:kirby:gen/files/" + spray_shown).c_str());
            host.publish_text(host.userdata, "spray.popbtn", ("module:kirby:gen/files/" + button).c_str());
        }
    }

    // Magolor Soul's second form on the True Arena's page (user's design 2026-11-01): its two sand tornadoes take the
    // page's elements away as they cross it (soul.txt's groups: the ribbon, the souvenir, Drop and Pause, then the boss's
    // card, the bar and the player's card), each element's place covered by the ground as it was, the page's background
    // standing still meanwhile; its giant lasers come down over the whole page, the ground burning away in the V between
    // them, the starry sky left; Magolor's words come and go over it; at 15% of its life the scream, then his hand circle,
    // whose tap gives Kirby the Ultra Sword; the sky stays after the Soul is beaten. The Soul's script context is
    // [[root+0x360]+0x6F0] (vtable main+0x15BEA80), its anim kind at [ctx+0x1540]+0x38, the anim's length at
    // [[[ctl]+0x848]+0x70]+0x48 (recorded 2026-11-01): 0x2f / 0x30 (130) its first hand's tornado, from the right / the
    // left, 0x33 / 0x34 (100) the other hand's, 0x3e (30) its giant lasers' shot, 0x40 (90) their end. Other bosses use
    // the same kinds for other anims, so the fight is told by the boss's name and the lengths.
    static constexpr int SoulElements = 7, SoulGround = 7, SoulWordSlots = 4, SoulPhrases = 4, SoulTornadoFrames = 8, SoulBeamLoop = 4,
                         SoulBeamThin = 6, SoulBurnFrames = 12;
    static constexpr u64 SoulReach = 21, SoulThin = 42, SoulBurn = 96, SoulScream = 132, SoulRingIn = 24, SoulRingOut = 21, SoulFlash = 24;
    // a tornado's coming after its hand's anim begins (thrown from the back, it reaches Kirby's plane later) and its
    // crossing, in ticks; user:soultime.txt ("<first> <second> <cross>" in seconds) overrides them, read once a second
    u64 soul_first_delay{96}, soul_second_delay{84}, soul_cross{144}, soul_time_read{};
    std::vector<KirbyFx::Group> soul_groups;
    bool soul_read{}, soul_frozen{}, soul_on{}, soul_warm{};
    struct SoulShot {
        std::array<std::vector<FxItem>, SoulElements + 1> items;
        std::u32string time;
    };
    std::mutex soul_mutex;
    std::map<int, SoulShot> soul_shots;
    int soul_next_id{1};
    std::array<int, 2> soul_shot{}, soul_dir{};
    u64 soul_ctx{}, soul_beam_at{}, soul_thin_at{}, soul_space_at{}, soul_scream_at{}, soul_tap_at{}, soul_done_at{}, soul_word_next{};
    std::array<u64, 2> soul_tor_at{};
    std::array<u64, SoulElements> soul_caught{};
    u32 soul_kind{~0u};
    float arena_life{-1}, soul_life{-1}, arena_bars{-1}; // the Soul's life (named on its panel); all the panels' fullest bar
    struct SoulWord {
        u64 at{}, life{};
        float x{}, y{}, tilt{};
        int phrase{}, cap{};
    };
    std::array<SoulWord, SoulWordSlots> soul_words{};
    std::mt19937 soul_random{static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::atomic<bool> soul_tap_pending{false}, soul_force_scream{false}; // (research: the scream without the 15%)

    // the words in the game's language (the module's fonts are Latin only: English for the others)
    struct SoulTexts {
        std::u32string_view kirby, help, help_kirby, sorry, scream, tap;
    };
    static SoulTexts SoulLines(std::string_view dir) {
        if (dir.ends_with("Italian"))
            return {U"Kirby...", U"Aiuto...", U"Aiutami, Kirby...", U"Scusami...", U"KIRBYYY!!!", U"Tocca!"};
        if (dir.ends_with("French"))
            return {U"Kirby...", U"À l'aide...", U"Aide-moi, Kirby...", U"Pardon...", U"KIRBYYY !!!", U"Touche !"};
        if (dir.ends_with("German"))
            return {U"Kirby...", U"Hilfe...", U"Hilf mir, Kirby...", U"Es tut mir leid...", U"KIRBYYY!!!", U"Tippen!"};
        if (dir.ends_with("Spanish"))
            return {U"Kirby...", U"Ayuda...", U"Ayúdame, Kirby...", U"Lo siento...", U"¡¡¡KIRBYYY!!!", U"¡Toca!"};
        if (dir.ends_with("Dutch"))
            return {U"Kirby...", U"Help...", U"Help me, Kirby...", U"Het spijt me...", U"KIRBYYY!!!", U"Tik!"};
        if (dir.ends_with("Portuguese"))
            return {U"Kirby...", U"Socorro...", U"Me ajuda, Kirby...", U"Me desculpa...", U"KIRBYYY!!!", U"Toque!"};
        return {U"Kirby...", U"Help...", U"Help me, Kirby...", U"I'm sorry...", U"KIRBYYY!!!", U"Tap!"};
    }

    static std::string SoulKey(int shot, const std::string& rest) {
        return "soul/" + std::to_string(shot) + "/" + rest;
    }

    // the shot of the elements one tornado takes, and the ground (the same in both: the background stands still); taken
    // at the tick's end (SoulCaptureNow), once every value of the page is published, its pictures drawn after that
    struct SoulWant {
        int id, first, last;
    };
    std::vector<SoulWant> soul_want;
    std::set<int> soul_have;
    int SoulCapture(int first, int last) {
        const int id = soul_next_id++;
        soul_want.push_back({id, first, last});
        return id;
    }
    void SoulCaptureNow(const EdenDsmodHostApi& host) {
        if (soul_want.empty() || !host.get_i64 || !host.get_text)
            return;
        for (const auto& want : soul_want) {
            SoulShot shot;
            for (const auto& g : soul_groups)
                if ((g.id >= want.first && g.id <= want.last) || g.id == SoulGround)
                    shot.items[static_cast<std::size_t>(g.id)] = CaptureItems(host, g.page);
            if (want.first <= 5 && want.last >= 5 && arena_time >= 0) {
                char clock[16];
                std::snprintf(clock, sizeof clock, "%ld:%02ld.%02ld", arena_time / 6000, arena_time / 100 % 60, arena_time % 100);
                shot.time = Ascii(clock);
            }
            {
                std::scoped_lock lock{soul_mutex};
                soul_shots[want.id] = std::move(shot);
            }
            soul_have.insert(want.id);
            FilesAhead(SoulKey(want.id, "ground"));
            for (int i = want.first; i <= want.last; ++i)
                FilesAhead(SoulKey(want.id, "piece/" + std::to_string(i))), FilesAhead(SoulKey(want.id, "patch/" + std::to_string(i)));
        }
        soul_want.clear();
    }

    void SoulReset() {
        for (const auto& [id, shot] : soul_shots) {
            for (int i = 0; i < SoulElements; ++i)
                FilesForget(SoulKey(id, "piece/" + std::to_string(i))), FilesForget(SoulKey(id, "patch/" + std::to_string(i)));
            FilesForget(SoulKey(id, "ground"));
            for (int f = 0; f < SoulBurnFrames; ++f)
                FilesForget(SoulKey(id, "v/" + std::to_string(f)));
        }
        {
            std::scoped_lock lock{soul_mutex};
            soul_shots.clear();
        }
        soul_want.clear(), soul_have.clear();
        soul_on = soul_frozen = false, soul_shot = {}, soul_dir = {}, soul_tor_at = {}, soul_caught = {}, soul_words = {};
        soul_beam_at = soul_thin_at = soul_space_at = soul_scream_at = soul_tap_at = soul_done_at = soul_word_next = 0;
        soul_tap_pending = false;
    }

    // soultornado/<f>, soulbeam/<f> (0..3 the loop, 4..7 dwindling), soulword/<dir>/<phrase>/<cap>, soulscream/<dir>, soulring,
    // soultap/<dir>, soul/<shot>/piece|patch/<element>, soul/<shot>/ground, soul/<shot>/v/<f>
    std::optional<KirbyAssets::Image> RenderSoul(const std::vector<std::string>& f) {
        const auto n = [&](std::size_t k) { return std::atoi(f[k].c_str()); };
        const auto tex = [&](const char* name) { return EffectTexture("MahoroaBattle2", name); };
        const auto centred = [](const std::optional<KirbyAssets::Image>& im, std::uint32_t w, std::uint32_t h) -> std::optional<KirbyAssets::Image> {
            if (!im)
                return std::nullopt;
            KirbyAssets::Image out{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)};
            KirbyFx::Draw(out, *im, static_cast<float>(w) / 2, static_cast<float>(h) / 2, static_cast<float>(im->width), static_cast<float>(im->height), 0, 1,
                          0xFFFFFFFF);
            return out;
        };
        if (f[0] == "soultornado" && f.size() == 2) {
            const auto scroll = tex("common_scrollsand01"), smoke = tex("sandsmoke01"), grains = tex("sand01"), edge = tex("windedge01");
            if (!scroll || !smoke || !grains || !edge)
                return std::nullopt;
            return KirbySoul::Tornado({scroll.get(), smoke.get(), grains.get(), edge.get()}, static_cast<float>(n(1)) / SoulTornadoFrames, 300, 520);
        }
        if (f[0] == "soulbeam" && f.size() == 2) {
            const auto scroll = tex("scroll11"), flash = tex("beamflash02");
            if (!scroll || !flash)
                return std::nullopt;
            static constexpr float Thin[SoulBeamThin]{0.18f, 0.36f, 0.52f, 0.67f, 0.8f, 0.92f};
            const int k = std::clamp(n(1), 0, SoulBeamLoop + SoulBeamThin - 1);
            return KirbySoul::LaserLeft({scroll.get(), nullptr, nullptr, flash.get()}, k < SoulBeamLoop ? 0 : Thin[k - SoulBeamLoop],
                                        k < SoulBeamLoop ? static_cast<float>(k) / SoulBeamLoop : 0, k, 360, 670);
        }
        if (f[0] == "soulword" && f.size() == 4) {
            const auto lines = SoulLines(f[1]);
            const std::u32string_view said[SoulPhrases]{lines.kirby, lines.help, lines.help_kirby, lines.sorry};
            const std::u32string text{said[std::clamp(n(2), 0, SoulPhrases - 1)]};
            auto words = KirbyModes::SoulWords(ModesSources(), text, static_cast<float>(n(3)));
            // (a long phrase smaller, to fit its slot)
            if (words && words->width > 640)
                words = KirbyModes::SoulWords(ModesSources(), text, std::floor(static_cast<float>(n(3)) * 630 / static_cast<float>(words->width)));
            return centred(words, 640, 120);
        }
        if (f[0] == "soulscream" && f.size() == 2)
            return centred(KirbyModes::SoulWords(ModesSources(), std::u32string{SoulLines(f[1]).scream}, 104), 1200, 240);
        if (f[0] == "soultap" && f.size() == 2)
            return centred(KirbyModes::PlateWords(ModesSources(), std::u32string{SoulLines(f[1]).tap}, 76), 520, 150);
        if (f[0] == "soulring") {
            const auto quarter = tex("mahoroabattle2handring");
            return quarter ? std::optional{KirbySoul::HandRing(*quarter, 800)} : std::nullopt;
        }
        if (f[0] != "soul" || f.size() < 3)
            return std::nullopt;
        SoulShot shot;
        {
            std::scoped_lock lock{soul_mutex};
            const auto it = soul_shots.find(n(1));
            if (it == soul_shots.end())
                return std::nullopt;
            shot = it->second;
        }
        const auto box = [&](int g) {
            for (const auto& gr : soul_groups)
                if (gr.id == g)
                    return gr.box;
            return std::array<int, 4>{0, 0, 1240, 1080};
        };
        if (f[2] == "ground")
            return DrawItems(shot.items[SoulGround]);
        if (f[2] == "piece" && f.size() == 4 && n(3) >= 0 && n(3) < SoulElements) {
            const int g = n(3);
            auto piece = DrawItems(shot.items[static_cast<std::size_t>(g)], box(g));
            // the bar's time, as the runtime's label draws it (KirbyFont's edged atlas, centred on x 400, its cap's top at 67)
            if (g == 5 && !shot.time.empty())
                if (const auto font = KirbyFont::BuildFontAtlas(romfs, "FOT-KurokaneStd-EB:45/002262", true)) {
                    float width = 0;
                    for (const char32_t c : shot.time)
                        width += font->Advance(c);
                    float pen = static_cast<float>(400 - static_cast<int>(width) / 2 - box(g)[0]);
                    const float baseline = static_cast<float>(67 + 45 - box(g)[1]);
                    for (const char32_t c : shot.time) {
                        const auto ref = font->Find(c);
                        if (!ref)
                            continue;
                        const auto& gl = *ref.glyph;
                        for (int y = 0; y < gl.h; ++y)
                            for (int x = 0; x < gl.w; ++x) {
                                const std::uint8_t* s = ref.Texel(x, y);
                                KirbyPaint::Over(piece, static_cast<std::uint32_t>(pen + gl.bearing_x + x), static_cast<std::uint32_t>(baseline - gl.bearing_y + y),
                                                 {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f});
                            }
                        pen += gl.advance;
                    }
                }
            return piece;
        }
        if (f[2] == "patch" && f.size() == 4 && n(3) >= 0 && n(3) < SoulElements) {
            // the ground where the element was (its shape grown 4 px)
            const int g = n(3);
            const auto piece = FilesImage(SoulKey(n(1), "piece/" + f[3]));
            const auto ground = FilesImage(SoulKey(n(1), "ground"));
            if (!piece || !ground)
                return std::nullopt;
            const auto b = box(g);
            KirbyAssets::Image out{piece->width, piece->height, std::vector<std::uint8_t>(piece->rgba.size())};
            const int w = static_cast<int>(piece->width), h = static_cast<int>(piece->height), r = 4;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    bool near = false;
                    for (int dy = -r; dy <= r && !near; dy += 2)
                        for (int dx = -r; dx <= r && !near; dx += 2) {
                            const int sx = std::clamp(x + dx, 0, w - 1), sy = std::clamp(y + dy, 0, h - 1);
                            near = piece->rgba[(std::size_t(sy) * w + sx) * 4 + 3] > 8;
                        }
                    const int px = b[0] + x, py = b[1] + y;
                    if (!near || px < 0 || py < 0 || px >= 1240 || py >= 1080)
                        continue;
                    std::memcpy(&out.rgba[(std::size_t(y) * w + x) * 4], &ground->rgba[(std::size_t(py) * 1240 + px) * 4], 4);
                }
            return out;
        }
        if (f[2] == "v" && f.size() == 4) {
            // the ground in the V between the beams burning, from its sides in (half size: 40 x 540 for 80 x 1080 at x 580)
            const auto ground = FilesImage(SoulKey(n(1), "ground"));
            const auto noise = tex("scroll23");
            if (!ground || !noise)
                return std::nullopt;
            const float since = static_cast<float>(n(3)) / (SoulBurnFrames - 1) * static_cast<float>(SoulBurn) / 60;
            KirbyAssets::Image out{40, 540, std::vector<std::uint8_t>(40 * 540 * 4)};
            for (int y = 0; y < 540; ++y)
                for (int x = 0; x < 40; ++x) {
                    const float px = 580 + 2 * static_cast<float>(x) + 1, py = 2 * static_cast<float>(y) + 1;
                    const float b = KirbySoul::Burnt(px, py, since, 1240, 1080, *noise);
                    if (b >= 1)
                        continue;
                    const std::uint8_t* g = &ground->rgba[(std::size_t(py) * 1240 + static_cast<std::size_t>(px)) * 4];
                    const KirbyPaint::Rgba c = KirbySoul::Burning({g[0] / 255.0f, g[1] / 255.0f, g[2] / 255.0f, 1}, {0, 0, 0, 0}, b);
                    const float a = b < 0.5f ? 1 : 1 - (b - 0.5f) * 2 * (b - 0.5f) * 2;
                    std::uint8_t* d = &out.rgba[(std::size_t(y) * 40 + x) * 4];
                    d[0] = static_cast<std::uint8_t>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255));
                    d[1] = static_cast<std::uint8_t>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255));
                    d[2] = static_cast<std::uint8_t>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255));
                    d[3] = static_cast<std::uint8_t>(std::lround(a * 255));
                }
            return out;
        }
        return std::nullopt;
    }

    void SoulStep(const EdenDsmodHostApi& host) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        const auto log = [&](const std::string& what) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod soul: " + what).c_str());
        };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        if (!soul_read) {
            soul_read = true;
            if (const auto t = romfs("file:soul.txt"))
                soul_groups = KirbyFx::ParseGroups(std::string_view{reinterpret_cast<const char*>(t->data()), t->size()});
        }
        const u64 now = play_tick;
        if (now >= soul_time_read + 60) {
            soul_time_read = now;
            if (const auto t = romfs("user:soultime.txt")) {
                float first{}, second{}, cross{};
                if (std::sscanf(std::string(t->begin(), t->end()).c_str(), "%f %f %f", &first, &second, &cross) == 3 && cross > 0)
                    soul_first_delay = static_cast<u64>(std::max(0.0f, first) * 60), soul_second_delay = static_cast<u64>(std::max(0.0f, second) * 60),
                    soul_cross = static_cast<u64>(cross * 60);
            }
        }
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        // the Soul's context while its fight runs (named on its panel once; the name goes between its two forms)
        const u64 ctx = arena_mode == 2 ? rd(rd(rd(host.main_base + 0x16325B8) + 0x360) + 0x6F0) : 0;
        const bool context = ctx && rd(ctx) == host.main_base + 0x15BEA80;
        if (context && arena_boss == "ExMahoroaBattle2")
            soul_ctx = ctx;
        const bool fight = context && ctx == soul_ctx && arena_mode == 2;
        u32 kind = ~0u;
        float length = 0;
        if (fight) {
            const u64 ctl = rd(ctx + 0x1540);
            kind = static_cast<u32>(rd(ctl + 0x38));
            if (const u64 z = rd(rd(rd(ctl) + 0x848) + 0x70))
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, z + 0x48, &length, 4);
        }
        if (arena_boss == "ExMahoroaBattle2")
            soul_life = arena_life;
        // out of the Arenas, or the fight's room left (lost, or won: the sky lasts until the results come): all as before
        if (soul_on && (arena_mode != 2 || !fight))
            SoulReset(), log("reset");
        if (fight && !soul_warm) {
            soul_warm = true;
            for (int k = 0; k < SoulTornadoFrames; ++k)
                FilesAhead("soultornado/" + std::to_string(k));
            for (int k = 0; k < SoulBeamLoop + SoulBeamThin; ++k)
                FilesAhead("soulbeam/" + std::to_string(k));
            for (int p = 0; p < SoulPhrases; ++p)
                for (const int cap : {40, 50, 60})
                    FilesAhead("soulword/" + title_dir + "/" + std::to_string(p) + "/" + std::to_string(cap));
            FilesAhead("soulscream/" + title_dir), FilesAhead("soulring"), FilesAhead("soultap/" + title_dir), FilesAhead("wsky");
        }
        if (!fight && !soul_on)
            soul_warm = false;
        const auto near_len = [&](float want) { return std::abs(length - want) < 2; };
        if (kind != soul_kind && fight) {
            if ((kind == 0x2f || kind == 0x30) && near_len(130) && !soul_tor_at[0]) {
                soul_on = soul_frozen = true;
                soul_dir[0] = kind == 0x2f ? -1 : 1, soul_tor_at[0] = now + soul_first_delay;
                soul_shot[0] = SoulCapture(0, 3);
                log("first tornado from the " + std::string{kind == 0x2f ? "right" : "left"});
            } else if ((kind == 0x33 || kind == 0x34) && near_len(100) && soul_tor_at[0] && !soul_tor_at[1]) {
                soul_dir[1] = -soul_dir[0], soul_tor_at[1] = now + soul_second_delay;
                soul_shot[1] = SoulCapture(4, 6);
                log("second tornado");
            } else if (kind == 0x3e && near_len(30) && soul_tor_at[0] && !soul_beam_at) {
                soul_beam_at = now;
                log("lasers");
                for (int k = 0; k < SoulBurnFrames && soul_have.contains(soul_shot[0]); ++k)
                    FilesAhead(SoulKey(soul_shot[0], "v/" + std::to_string(k)));
            } else if (kind == 0x40 && near_len(90) && soul_beam_at && !soul_thin_at) {
                soul_thin_at = now;
                log("lasers ending");
            }
        }
        soul_kind = fight ? kind : ~0u;
        const auto smooth = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); };
        const auto alpha16 = [](float a) { return static_cast<std::int64_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 15)); };
        const auto shown = [&](const std::string& key) { return FilesReady(key, true) && FilesServed(key); };
        std::vector<std::string> ahead;
        // the tornadoes: each crossing in SoulCross ticks; an element is shown as its picture (its ground behind) from when the
        // funnel comes near, shaken, then caught: pulled along, lifted, spun, shrinking, fading
        float tor_x[2]{-10000, -10000};
        for (int k = 0; k < 2; ++k) {
            const bool on = soul_tor_at[k] && now >= soul_tor_at[k] && now <= soul_tor_at[k] + soul_cross && !soul_beam_at;
            const float s = soul_tor_at[k] && now >= soul_tor_at[k] ? static_cast<float>(now - soul_tor_at[k]) / static_cast<float>(soul_cross) : -1;
            const float from = soul_dir[k] > 0 ? -380.0f : 1620.0f, to = soul_dir[k] > 0 ? 1620.0f : -380.0f;
            if (s >= 0 && s <= 1)
                tor_x[k] = from + (to - from) * s;
            const std::string frame = "soultornado/" + std::to_string(now / 4 % SoulTornadoFrames);
            pub("soul.tor_on." + std::to_string(k), on && shown(frame) ? 1 : 0);
            text("soul.tor." + std::to_string(k), frame);
            pub("soul.tor." + std::to_string(k) + ".x", std::lround(tor_x[k] - 620));
            if (soul_tor_at[k] && !on && now < soul_tor_at[k])
                for (int f = 0; f < SoulTornadoFrames; ++f)
                    ahead.push_back("soultornado/" + std::to_string(f));
        }
        bool any_loose = false;
        for (int i = 0; i < SoulElements; ++i) {
            const int k = i < 4 ? 0 : 1;
            const auto& b = soul_groups.size() > static_cast<std::size_t>(i) ? soul_groups[static_cast<std::size_t>(i)].box : std::array<int, 4>{};
            const float ex = static_cast<float>(b[0]) + static_cast<float>(b[2]) / 2;
            const std::string id = std::to_string(i);
            const bool shot = soul_shot[k] && soul_have.contains(soul_shot[k]);
            const std::string piece = shot ? SoulKey(soul_shot[k], "piece/" + id) : "", patch = shot ? SoulKey(soul_shot[k], "patch/" + id) : "";
            float near = 0;
            if (tor_x[k] > -9999) {
                near = std::clamp(1 - std::abs(tor_x[k] - ex) / 420, 0.0f, 1.0f);
                const bool past = soul_dir[k] < 0 ? tor_x[k] <= std::min(ex + 60, 1240.0f) : tor_x[k] >= std::max(ex - 60, 0.0f);
                if (past && !soul_caught[static_cast<std::size_t>(i)])
                    soul_caught[static_cast<std::size_t>(i)] = now;
            }
            const u64 caught = soul_caught[static_cast<std::size_t>(i)];
            const bool ready = !piece.empty() && shown(piece) && shown(patch);
            const bool loose = ready && !soul_beam_at && (near > 0 || caught);
            any_loose |= loose;
            if (!piece.empty() && !ready)
                ahead.push_back(piece), ahead.push_back(patch);
            float dx = 0, dy = 0, deg = 0, scale = 1, alpha = loose ? 1 : 0;
            if (caught) {
                const float s = static_cast<float>(now - caught) / 60, dir = static_cast<float>(soul_dir[k]);
                dx = dir * (380 * s + 500 * s * s), dy = -(260 * s + 700 * s * s), deg = -dir * (420 * s + 700 * s * s);
                scale = 1 - 0.45f * std::min(s / 1.2f, 1.0f), alpha *= 1 - smooth((s - 0.7f) / 0.5f);
            } else {
                deg = near * 6 * std::sin(static_cast<float>(now) * 0.67f);
            }
            // (kept until the reset: the lasers come down over the page long before the sky covers it)
            pub("soul.patch_on." + id, loose || (ready && caught) ? 1 : 0);
            text("soul.patch." + id, patch.empty() ? "fx/white" : patch);
            text("soul.piece." + id, piece.empty() ? "fx/white" : piece);
            pub("soul.e" + id + ".x", std::lround(dx)), pub("soul.e" + id + ".y", std::lround(dy)), pub("soul.e" + id + ".r", std::lround(deg));
            pub("soul.e" + id + ".s", std::lround(scale * 1000)), pub("soul.e" + id + ".a", alpha16(alpha));
        }
        // the lasers: down in SoulReach ticks (the pictures sliding from above), the sky under them once they cover the
        // page, the V between them burning; dwindling at their end; then the words
        float reach = 0;
        if (soul_beam_at) {
            reach = smooth(static_cast<float>(now - soul_beam_at) / SoulReach);
            const u64 thin = soul_thin_at ? now - soul_thin_at : 0;
            const bool on = !soul_thin_at || thin < SoulThin;
            const std::string beam =
                "soulbeam/" + std::to_string(soul_thin_at ? SoulBeamLoop + std::min<u64>(SoulBeamThin - 1, thin * SoulBeamThin / SoulThin) : now / 4 % SoulBeamLoop);
            pub("soul.beam_on", on && shown(beam) ? 1 : 0);
            text("soul.beam", beam);
            // (dwindling in steps, fading every tick)
            pub("soul.beam.a", alpha16(soul_thin_at ? 1 - smooth(static_cast<float>(thin) / SoulThin) : 1));
            pub("soul.beam.y", -std::lround((1 - reach) * 1340));
            if (soul_thin_at && thin >= SoulThin && !soul_space_at)
                soul_space_at = now, log("space");
        } else {
            pub("soul.beam_on", 0), text("soul.beam", "soulbeam/0"), pub("soul.beam.y", -1340), pub("soul.beam.a", 15);
        }
        if (soul_tor_at[1] && !soul_space_at)
            for (int f = 0; f < SoulBeamLoop + SoulBeamThin; ++f)
                ahead.push_back("soulbeam/" + std::to_string(f));
        const bool sky = soul_beam_at && reach >= 1;
        pub("soul.sky", sky ? 1 : 0);
        const u64 burnt = sky ? now - soul_beam_at - SoulReach : 0;
        const std::string v = SoulKey(soul_shot[0], "v/" + std::to_string(std::min<u64>(SoulBurnFrames - 1, burnt * SoulBurnFrames / SoulBurn)));
        pub("soul.v_on", sky && burnt <= SoulBurn && soul_have.contains(soul_shot[0]) && shown(v) ? 1 : 0);
        text("soul.v", v);
        // the words over the sky, at random places and times, kept off the middle once the scream came
        const auto lines = SoulLines(title_dir);
        if (soul_space_at && !soul_done_at && now >= soul_word_next) {
            std::uniform_real_distribution<float> u(0, 1);
            for (auto& w : soul_words)
                if (!w.at || now > w.at + w.life) {
                    SoulWord n{now, static_cast<u64>(120 + 60 * u(soul_random)), 0, 0, (u(soul_random) - 0.5f) * 14, static_cast<int>(u(soul_random) * SoulPhrases),
                               40 + 10 * static_cast<int>(u(soul_random) * 3)};
                    for (int tries = 0; tries < 30; ++tries) {
                        n.x = 180 + u(soul_random) * 880, n.y = 90 + u(soul_random) * 900;
                        bool clash = soul_scream_at && n.x > 220 && n.x < 1020 && n.y > 260 && n.y < 820;
                        for (const auto& o : soul_words)
                            clash |= o.at && now <= o.at + o.life && std::abs(o.x - n.x) < 330 && std::abs(o.y - n.y) < 90;
                        if (!clash)
                            break;
                    }
                    w = n;
                    break;
                }
            soul_word_next = now + static_cast<u64>(54 + 66 * u(soul_random));
        }
        for (int k = 0; k < SoulWordSlots; ++k) {
            const auto& w = soul_words[static_cast<std::size_t>(k)];
            const std::string key = "soulword/" + title_dir + "/" + std::to_string(w.phrase) + "/" + std::to_string(w.cap ? w.cap : 40);
            float a = 0;
            if (w.at && now <= w.at + w.life && shown(key)) {
                const float s = static_cast<float>(now - w.at);
                a = std::min(smooth(s / 36), 1 - smooth((s - static_cast<float>(w.life) + 48) / 48));
            }
            const std::string n = "soul.w." + std::to_string(k);
            text(n, key), pub(n + ".x", std::lround(w.x - 320)), pub(n + ".y", std::lround(w.y - 60)), pub(n + ".r", std::lround(w.tilt));
            pub(n + ".a", alpha16(a));
        }
        // at 15% of its life: the scream, then the circle (the tap: the Ultra Sword; beaten: the circle goes by itself)
        if (soul_space_at && !soul_scream_at && ((soul_life >= 0 && soul_life <= 0.15f) || soul_force_scream.exchange(false)))
            soul_scream_at = now, log("scream");
        if (soul_space_at && !soul_done_at && arena_bars >= 0 && arena_bars <= 0.001f)
            soul_done_at = now, log("beaten");
        {
            const float s = soul_scream_at ? static_cast<float>(now - soul_scream_at) : -1;
            const bool on = s >= 0 && s < SoulScream;
            const float scale = s < 12 ? 0.3f + 0.85f * smooth(s / 12) : 1 + 0.06f * std::sin(6.2832f * 7 * s / 60);
            const int jx = static_cast<int>((now * 7919) % 17) - 8, jy = static_cast<int>((now * 104729) % 13) - 6;
            text("soul.scream", "soulscream/" + title_dir);
            pub("soul.scream.x", on ? jx : 0), pub("soul.scream.y", on ? jy : 0), pub("soul.scream.r", on ? static_cast<int>(now % 7) - 3 : 0);
            pub("soul.scream.s", std::lround(scale * 1000)), pub("soul.scream.a", alpha16(on && shown("soulscream/" + title_dir) ? 1 - smooth((s - 114) / 18) : 0));
        }
        {
            const u64 ring_at = soul_scream_at ? soul_scream_at + SoulScream - 36 : 0;
            const float s = ring_at && now >= ring_at ? static_cast<float>(now - ring_at) : -1;
            const u64 out_at = soul_tap_at ? soul_tap_at : soul_done_at;
            const float gone = out_at && ring_at && out_at >= ring_at ? smooth(static_cast<float>(now - out_at) / SoulRingOut) : 0;
            const bool ready = shown("soulring") && shown("soultap/" + title_dir);
            const float in = s >= 0 && ready ? smooth(s / SoulRingIn) : 0, glow = 0.85f + 0.15f * std::sin(6.2832f * 1.2f * s / 60);
            const float swell = soul_tap_at ? 1 + 0.4f * gone : 1;
            pub("soul.ring.r", s >= 0 ? -std::lround(s * 20 / 60) : 0);
            pub("soul.ring.s", std::lround((0.6f + 0.4f * in) * swell * 1000));
            pub("soul.ring.a", alpha16(in * glow * (1 - gone)));
            text("soul.tap", "soultap/" + title_dir);
            pub("soul.tap.s", std::lround((1 + 0.05f * std::sin(6.2832f * 1.5f * s / 60)) * in * (1 + 0.3f * (soul_tap_at ? gone : 0)) * 1000));
            pub("soul.tap.a", alpha16(in * (1 - gone)));
            const bool can = in >= 1 && !soul_tap_at && !soul_done_at;
            pub("soul.can_tap", can ? 1 : 0);
            if (soul_tap_pending.exchange(false) && can) {
                soul_tap_at = now, log("circle tapped");
                cheat_pending = 28, cheat_tries = 0;
            }
            const float flash = soul_tap_at ? 1 - static_cast<float>(now - soul_tap_at) / SoulFlash : 0;
            pub("soul.flash", alpha16(flash * 0.6f));
        }
        pub("soul.block", soul_on && soul_tor_at[0] && now >= soul_tor_at[0] ? 1 : 0);
        pub("soul.calm", soul_on ? 0 : 1);
        // the pictures wanted next handed to the host through the hidden widgets, a few at a time
        if (soul_beam_at && !soul_space_at && soul_have.contains(soul_shot[0]))
            for (int f = 0; f < SoulBurnFrames; ++f)
                ahead.push_back(SoulKey(soul_shot[0], "v/" + std::to_string(f)));
        if (soul_thin_at || soul_space_at) {
            for (int p = 0; p < SoulPhrases; ++p)
                for (const int cap : {40, 50, 60})
                    ahead.push_back("soulword/" + title_dir + "/" + std::to_string(p) + "/" + std::to_string(cap));
            ahead.push_back("soulscream/" + title_dir), ahead.push_back("soulring"), ahead.push_back("soultap/" + title_dir);
        }
        std::erase_if(ahead, [&](const std::string& key) { return !FilesReady(key) || FilesServed(key); });
        pub("soul.ahead", ahead.empty() ? 0 : 1);
        for (int k = 0; k < 6; ++k)
            text("soul.next." + std::to_string(k), ahead.empty() ? "fx/white" : ahead[(static_cast<std::size_t>(now / 6) * 6 + k) % ahead.size()]);
    }

    void ArenaStep(const EdenDsmodHostApi& host) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        ArenaBossStep(host);
        if (play_on)
            CheatStep(host);
        // The VS scene (user's design 2026-10-24): going through the rest room's door to a fight the page fades but for
        // the two cards; they draw back, rush at each other and clash, the VS burst bumps up and sparks run round the
        // frames (KirbyModes::VsScene). When the fight's room is up they fade out, the boss's card goes back off the
        // page unseen, and the page is as usual again. Only through the door: the game holds Kirby while he walks in (a
        // return to the title from the pause menu leaves the room too); a fight's room that never comes ends it after 10 s
        if (play_on && play_posing && arena_rest)
            arena_door_at = play_tick;
        if (vs_state == VsNone && arena_was_on && !play_on && arena_rest && arena_card_on && arena_door_at && play_tick - arena_door_at <= 90)
            vs_state = VsPlaying, vs_at = play_tick;
        if (vs_state == VsPlaying && (play_on || play_tick - vs_at > 600))
            vs_state = VsFading, vs_end = play_tick;
        if (vs_state == VsFading && play_tick - vs_end >= static_cast<u64>(KirbyModes::VsFadeTicks))
            vs_state = VsClearing, vs_gone = play_tick, arena_card_on = false, arena_card.clear(), arena_card_off = play_tick;
        if (vs_state == VsClearing && play_tick - vs_gone >= ArenaCardTicks)
            vs_state = VsNone;
        const bool vs = vs_state == VsPlaying || vs_state == VsFading;
        const auto pose = vs ? KirbyModes::VsScene(static_cast<float>(play_tick - vs_at)) : KirbyModes::VsPose{};
        const float fade = vs_state == VsFading ? 1 - static_cast<float>(play_tick - vs_end) / KirbyModes::VsFadeTicks : vs_state == VsClearing ? 0 : 1;
        pub("arena.vs.alpha", std::clamp(std::lround(fade * 15), 0L, 15L));
        pub("arena.p.dx", std::lround(pose.player[0])), pub("arena.p.dy", std::lround(pose.player[1]));
        pub("arena.b.dx", std::lround(pose.boss[0])), pub("arena.b.dy", std::lround(pose.boss[1]));
        pub("arena.pcard_on", vs ? 1 : vs_state == VsClearing ? 0 : play_on ? 1 : 0);
        pub("arena.vs_on", vs && pose.burst > 0 ? 1 : 0);
        pub("arena.vs.scale", std::lround(pose.burst * 1000));
        pub("arena.sparks_on", vs && pose.sparks ? 1 : 0);
        // the figure's frames: the one shown now, the rest ahead
        // (true once the frame shown is the figure's own; else the point keeps its last picture, published again: a
        // picture not published on a tick goes)
        const auto loop = [&](const std::string& base, const std::string& point) {
            for (int i = 0; i < ArenaFrames; ++i)
                FilesAhead(base + std::to_string(i));
            const u64 ticks = static_cast<u64>(ArenaLoopTicks(base));
            const int frame = static_cast<int>(play_tick % ticks * ArenaFrames / ticks);
            // only the last three figures' pictures are kept (Kirby's, the boss's, the one going): the rest forgotten
            if (arena_recent.empty() || arena_recent.front() != base) {
                std::erase(arena_recent, base);
                arena_recent.push_front(base);
                for (; arena_recent.size() > 3; arena_recent.pop_back())
                    for (int i = 0; i < ArenaFrames; ++i)
                        FilesForget(arena_recent.back() + std::to_string(i));
            }
            if ((!play_on && !vs) || !FilesReady(base + std::to_string(frame))) {
                if (const auto it = arena_shown.find(point); it != arena_shown.end())
                    text(point, it->second);
                return false;
            }
            text(point, arena_shown[point] = base + std::to_string(frame));
            return true;
        };
        // Kirby with his abilities, or the other characters (Meta Knight 35, King Dedede 36, Bandana Waddle Dee 37)
        // (the character is kept through a door and a room's fade, where the ability reads as none, and on Landia in the
        // Lor's fight, ability 27 for every character)
        if (play_on && play_ability >= 1 && play_ability != 27)
            arena_character = play_ability == 35 ? "Metaknight" : play_ability == 36 ? "Dedede" : play_ability == 37 ? "Waddledee" : "Kirby";
        const bool figure = arena_character != "Kirby" || play_ability <= 32;
        const std::string ability = arena_character == "Metaknight" ? "Meta"
                                  : arena_character == "Dedede"     ? "Dedede"
                                  : arena_character == "Waddledee"  ? "Dee"
                                  : play_on && play_ability >= 1 && figure ? KirbyModes::AbilityKey(static_cast<int>(play_ability))
                                                                           : "Normal";
        // (the figure keeps its ability and goes on through the VS scene; through a door, a scene of Kirby's and the VS
        // the ability may read as none for a while: a plain Kirby only after half a second of it)
        if (ability != "Normal" || !play_on)
            arena_plain_since = play_tick;
        const bool plain_now = ability != "Normal" || play_tick - arena_plain_since >= 30 || arena_figure.empty();
        if (play_on && figure && !play_posing && vs_state == VsNone && plain_now)
            arena_figure = "arenakirby/" + ability + (KirbyChara::CharacterFolder(ability).empty() ? FigureToken() : "") + "/";
        if (vs ? !arena_figure.empty() : play_on && figure)
            loop(arena_figure, "arena.kirby");
        else {
            if (!play_on)
                for (int i = 0; i < ArenaFrames; ++i)
                    FilesAhead("arenakirby/Normal" + FigureToken() + "/" + std::to_string(i));
            if (const auto it = arena_shown.find("arena.kirby"); it != arena_shown.end())
                text("arena.kirby", it->second);
        }
        // (drawn ahead from the rest room on)
        if (arena_rest || vs) {
            FilesAhead("vsburst/0");
            for (int which = 0; which < 3; ++which)
                for (int f = 0; f < KirbyModes::VsSparkFrames; ++f)
                    FilesAhead("vssparks/" + std::to_string(which) + "/" + std::to_string(f));
        }
        if (FilesReady("vsburst/0"))
            text("arena.vs", "vsburst/0");
        for (int which = 0; which < 3; ++which)
            if (const std::string key = "vssparks/" + std::to_string(which) + "/" + std::to_string(play_tick / 3 % KirbyModes::VsSparkFrames); vs && FilesReady(key))
                text("arena.spark." + std::to_string(which), key);
        pub("arena.kirby_on", (play_on || vs) && figure ? 1 : 0);
        text("arena.plate.0", "arenaplate/" + title_dir + "/0/" + arena_character + (arena_character == "Kirby" ? PlateToken() : ""));
        // The boss's card (panel, picture, plate) slides in from the right once its picture is drawn, and out before
        // another boss takes it: the card keeps the boss it shows until it is off the page
        // (the VS scene keeps the card's boss, the next one, through the room's fade)
        const std::string want = vs ? arena_card : vs_state == VsClearing ? std::string{} : play_on ? arena_boss : std::string{};
        if (arena_card != want) {
            if (arena_card_on)
                arena_card_on = false, arena_card_off = play_tick;
            else if (play_tick - arena_card_off >= ArenaCardTicks)
                arena_card = want, arena_card_name = arena_boss_name;
        }
        if (!arena_card.empty() && loop("arenaboss/" + arena_card + "/", "arena.boss")) {
            arena_shown["arena.plate.1"] = "arenaplate/" + title_dir + "/1/" + arena_card_name;
            if (arena_card == want)
                arena_card_on = true;
        }
        if (const auto it = arena_shown.find("arena.plate.1"); it != arena_shown.end())
            text("arena.plate.1", it->second);
        FilesAhead("arenapanel/0"), FilesAhead("arenapanel/1");
        if (FilesReady("arenapanel/0") && FilesReady("arenapanel/1"))
            text("arena.panel.0", "arenapanel/0"), text("arena.panel.1", "arenapanel/1");
        pub("arena.card_on", arena_card_on ? 1 : 0);
        const auto [charge, charging] = BatteryCharge(host);
        SwapStep(host, arena_bar,
                 "arenabar/" + title_dir + "/" + std::to_string(play_on ? play_ability : 0) + "/" + std::to_string(charge) +
                     (charging ? "c" : ""),
                 "arena.bar", "arena.bar_next", play_tick, Draw::Urgent);
        pub("arena.battery", charge >= 0 ? 1 : 0);
        // the run's time: u32 frames at [[main+0x16325B8]+0x1B8]+0x1E4 (validated against the rest room's board), read in
        // play only and kept through loads, written as the board writes it
        const auto rd = [&](u64 at, auto& out) { return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &out, sizeof out); };
        u64 root{}, run{};
        u32 frames{};
        if (play_on && rd(host.main_base + 0x16325B8, root) && rd(root + 0x1B8, run) && run && rd(run + 0x1E4, frames) && frames < 360000)
            arena_time = frames * 100L / 60;
        char clock[16]{};
        if (arena_time >= 0)
            std::snprintf(clock, sizeof clock, "%ld:%02ld.%02ld", arena_time / 6000, arena_time / 100 % 60, arena_time % 100);
        host.publish_text(host.userdata, "arena.time", clock);
        SoulStep(host);
        arena_was_on = play_on;
    }

    void EpilogueStep(const EdenDsmodHostApi& host) {
        ++epi_tick;
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        const auto text = [&](const std::string& name, const std::string& v) {
            host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + v).c_str());
        };
        if (const auto save = EpiSaveOf(host, static_cast<int>(std::min(files_cursor, 2u))))
            epi_save = *save, epi_save_ok = true;
        EpiPrewarm(host, static_cast<int>(std::min(files_cursor, 2u)), true);
        // the game's menus and loads lock the page, as on the stage page
        u64 root{};
        u32 menu[2]{};
        const bool read = dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x16325B8, &root, 8) &&
                          dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, root + 0x378, menu, sizeof menu);
        const bool loading = !read || (menu[1] >> 8) != 0x7FBFFF || (menu[1] & 0xFF) == 0xFF;
        // as on the stage page: a menu, a load or a scene of the game's own (Magolor taking a door, a boss's
        // intro, his defeat: the player record's hold) lock the page, unless it lasts under 4 ticks (the game
        // freezes the action for a few frames on a hit)
        const bool paused = !loading && menu[0] == 1;
        const std::string held = loading ? std::string{} : MagolorScene(host, root);
        if (held != epi_held && host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod epi: Magolor " + (held.empty() ? std::string{"free"} : "held (" + held + ")")).c_str());
        epi_held = held;
        const bool busy = loading || menu[0] != 0 || play_posing || !held.empty();
        epi_busy = busy ? epi_busy + 1 : 0;
        const bool free = epi_busy < 4;
        if (!loading)
            EpilogueHud(host, root);
        // the page shows in play (the stage HUD up, not loading), not on the Epilogue's own title screen: that
        // is the altar's room with its menu open (menu 2), so play starts once no menu has been open for 20 ticks
        epi_free = !busy && play_hud ? epi_free + 1 : 0;
        if (epi_free >= 20)
            epi_started = true;
        // "back to the title" from the pause menu reloads that room with the menu open: the page waits again
        if (!loading && menu[0] == 2)
            epi_started = false;
        // after a load (and on entering) the HUD's gauge stays empty until Magolor can move, then refills to the
        // saved level: the ring shows the save's until the HUD is back to it (the first ticks after a load read what
        // is left of the old room's gauge; 20 s at most)
        if (loading)
            epi_settled = false, epi_loaded = epi_tick;
        else if (!epi_settled && epi_save_ok && epi_tick - epi_loaded > 15) {
            // (or refilled to less than the save says, and still for 15 ticks)
            if (epi_gauge != epi_seen)
                epi_seen = epi_gauge, epi_seen_at = epi_tick;
            epi_settled = epi_save.gauge == 0 || epi_gauge >= epi_save.gauge - 3 || (epi_gauge > 0 && epi_tick - epi_seen_at >= 15) ||
                          epi_tick - epi_loaded > 1200;
        }
        // between two rooms (Magolor's objects gone, as on the stage page) only the background shows
        const bool live = epi_started && play_hud && !loading && !play_gone;
        epi_live = live;
        // what the page shows as learnt: the save's, but a change only in the hub once Magolor has been free there for
        // half a second, so a boss's reward shows back in the hub as he gets control again, not in the boss's room
        const bool hub = EpiInHub(host, root);
        if (hub != epi_hub && host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, hub ? "Kirby DSMod epi: in the hub" : "Kirby DSMod epi: out of the hub");
        epi_hub = hub;
        const EpiLearnt learnt{epi_save.cannon, epi_save.hole, epi_save.heal, epi_save.blast};
        if (!epi_learnt_ok && epi_save_ok)
            epi_learnt = learnt, epi_learnt_ok = true;
        else if (learnt == epi_learnt || !live || busy || !hub)
            epi_learnt_wait = 0;
        else if (++epi_learnt_wait >= 30)
            epi_learnt = learnt, epi_learnt_wait = 0;
        const int shown = epi_settled ? epi_gauge : epi_save.gauge;
        const bool full = epi_learnt.cannon && shown >= 360 && epi_smooth >= 359.5f;
        // a magic's popup, once per file (persisted flags): over the locked page as soon as it shows with the magic
        // learnt, until its button (its push played) or the next load, a room's or the mode's end
        // (in the hub only, and only once its pictures are drawn, so it shows whole)
        const auto slot = static_cast<std::size_t>(std::min(files_cursor, 2u));
        if (epi_pop && (loading || play_gone || (epi_pop_close && epi_tick - epi_pop_close >= 8)))
            PopupSeen();
        else if (!epi_pop && live && hub)
            if (const int due = PopupDue({0, 0, 0, epi_learnt.cannon, epi_learnt.hole, epi_learnt.heal, epi_learnt.blast}, static_cast<int>(slot));
                due && FilesReady(PopupKey(due), true) && FilesReady("epipopbtn/" + title_dir, true))
                epi_pop = due;
        if (epi_pop_tap.exchange(false) && epi_pop && live && !epi_pop_close)
            epi_pop_close = epi_pop_pushed = epi_tick;
        if (host.log) {
            u64 scene{}, vt{};
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, host.main_base + 0x1642EE8, &scene, 8);
            if (scene)
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, scene, &vt, 8);
            const std::string state = "hud=" + std::to_string(play_hud) + " gone=" + std::to_string(play_gone) + " posing=" + std::to_string(play_posing) + " loading=" + std::to_string(loading) + " menu=" +
                                      std::to_string(menu[0]) + " scene_vt=main+" + KirbyHex(vt ? vt - host.main_base : 0) +
                                      " gauge_layout=" + std::to_string(epi_tick - epi_hud_at < 30) + " gauge=" + std::to_string(epi_gauge) + " saved=" + std::to_string(epi_save.gauge) +
                                      " settled=" + std::to_string(epi_settled) + " srt=" + std::to_string(epi_shown) + " learnt=" +
                                      std::to_string(epi_save.cannon) + std::to_string(epi_save.hole) + std::to_string(epi_save.heal) + "/" +
                                      std::to_string(epi_learnt.cannon) + std::to_string(epi_learnt.hole) + std::to_string(epi_learnt.heal) +
                                      " pop=" + std::to_string(epi_pop);
            if (state != epi_state)
                epi_state = state, host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod epi " + state).c_str());
        }
        if (full && epi_full_at == 0)
            epi_full_at = epi_tick;
        else if (!full)
            epi_full_at = 0;
        // the special moves: X with a full gauge, Up + X for Black Hole
        const auto can = [&](int i) {
            // 0 pause, 1 Magolor Cannon, 2 Black Hole, 3..5 the tabs, 6 Heal, 7 Magolor Blast
            const bool idle = heal_phase == 0 && blast_phase == 0;
            return live && free && !epi_pop &&
                   (i == 0   ? epi_tab == 0
                    : i == 1 ? epi_tab == 0 && full && idle
                    : i == 2 ? epi_tab == 0 && full && epi_learnt.hole && idle
                    : i == 6 ? epi_tab == 0 && full && epi_learnt.heal && idle
                    : i == 7 ? epi_tab == 0 && full && epi_learnt.blast && idle
                    // (from the last blow's charge on the page stays on the Game tab: the gauge shows)
                    : i == 4 || i == 5 ? fin_phase < FinCharge
                             : true);
        };
        HealStep(host);
        BlastStep(host);
        EpiCheatStep(host, root, live && free && !epi_pop && heal_phase == 0 && blast_phase == 0);
        if (live && epi_learnt.blast)
            ScanAttackTable(host, 2048);
        if (const int tap = epi_tap.exchange(-1); tap >= 0 && tap < 8 && can(tap)) {
            epi_pushed[tap] = epi_tick;
            if (tap == 0)
                Press("plus");
            else if (tap == 1)
                Press("x");
            else if (tap == 2)
                Press("upx");
            else if (tap == 6) {
                if (heal_phase == 0 && HealPatch(host, true))
                    heal_phase = 1, heal_at = epi_tick, Press("x");
                else if (host.log)
                    host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod heal: the step scripts were not found as expected");
            } else if (tap == 7) {
                if (blast_phase == 0 && StepPatch(host, BlastPatches, true)) {
                    if (HeroAnimNames(host, BlastNames, true)) {
                        blast_phase = 1, blast_at = epi_tick, Press("x");
                        if (!BlastDamage(host, true) && host.log)
                            host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod blast: the attack table was not found, the burst's own damage");
                    } else
                        StepPatch(host, BlastPatches, false);
                }
                if (blast_phase == 0 && host.log)
                    host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod blast: the step scripts or Magolor's animations were not found as expected");
            }
            else if (tap - 3 != epi_tab)
                epi_tab_from = epi_tab, epi_tab = tap - 3, epi_tab_at = epi_tick, epi_boost = !epi_boost;
        }
        const bool resumable = paused && !epi_pop;
        if (resumable && !epi_resumable)
            epi_resume_pushed = epi_tick;
        epi_resumable = resumable;
        if (epi_resume_tap.exchange(false) && resumable && epi_tab == 0)
            epi_resume_pushed = epi_tick, Press("plus");
        const auto push = [&](u64 at) {
            float scale = 1;
            if (const float q = static_cast<float>(epi_tick - at); epi_tick >= at && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            return std::lround(scale * 1000);
        };
        for (int i = 0; i < 8; ++i) {
            pub("epi.scale." + std::to_string(i), push(epi_pushed[i]));
            pub("epi.can." + std::to_string(i), can(i) ? 1 : 0);
        }
        pub("epi.scale.resume", push(epi_resume_pushed));
        // Continue in the Game tab only (it slides away with it)
        pub("epi.paused", resumable ? 1 : 0);
        pub("epi.resume", resumable && epi_tab == 0 ? 1 : 0);
        pub("epi.vis", live ? 1 : 0);
        pub("epi.lock", live && (!free || epi_pop) ? 1 : 0);
        pub("epi.popup", live && epi_pop ? 1 : 0);
        pub("epi.scale.pop", push(epi_pop_pushed));
        // (the last one shown stays through its fade-out)
        if (epi_pop)
            epi_pop_shown = epi_pop;
        if (epi_pop_shown)
            text("epi.pop", PopupKey(epi_pop_shown)), text("epi.popbtn", "epipopbtn/" + title_dir);
        pub("epi.boost", epi_boost ? 1 : 0);
        pub("epi.in_game", epi_tab == 0 ? 1 : 0);
        // section i sits (i - tab) screens to the right, as on the stage page
        const auto section = [&](int i) {
            const float from = static_cast<float>(i - epi_tab_from), to = static_cast<float>(i - epi_tab);
            return std::lround((from + (to - from) * Ease(epi_tick - epi_tab_at, SectionFrames)) * SectionShift);
        };
        pub("epi.sx", section(0));
        OptionsStep(host, epo, true, epi_tick, epi_tab == 2, static_cast<float>(section(2)), live && free && !epi_pop, epi_boost);
        const std::string moves = EpiMoves(epi_save), magic = title_dir + "/" + moves;
        const auto content = MagolorOf(title_dir, moves, false);
        PagerStep(host, epim, "epim", magic, content ? std::max<int>(1, static_cast<int>(content->pages.size())) : 1,
                  [&](int p) { return "epimpage/" + magic + "/" + std::to_string(p); },
                  "epiminfo/" + title_dir + "/" + (EpiLevel(epi_save) >= EpiLevels ? "1" : "0"), epi_tick, epi_tab == 1,
                  live && free && !epi_pop, section(1), epi_boost);
        // the strip of learnt techniques under the description, looping a pixel a tick: cells a pitch apart, each
        // showing the technique that has come to it; outside the screen's own place a cell crossing the section's
        // edge hides, as it would show over the next section
        std::vector<std::string> cells;
        for (const int t : KirbyModes::TechniqueMenuOrder)
            if (const int l = epi_save.levels[static_cast<std::size_t>(t)]; l > 0)
                cells.push_back("epimtech/" + std::to_string(t) + "/" + std::to_string(l));
        const long sx1 = section(1), pitch = static_cast<long>(KirbyModes::TechniqueCellSize[0]);
        const long offset = cells.empty() ? 0 : static_cast<long>(epi_tick % (cells.size() * static_cast<u64>(pitch)));
        for (int i = 0; i < EpiStripCells; ++i) {
            const std::string cell = "epim.cell." + std::to_string(i);
            const long x = sx1 + i * pitch - offset % pitch;
            const std::string& key = cells.empty() ? std::string{} : cells[(static_cast<std::size_t>(offset / pitch) + i) % cells.size()];
            if (!key.empty() && (epi_tab == 1 ? FilesReady(key) : (FilesAhead(key), FilesDone(key))))
                text(cell, key);
            pub(cell + ".x", x);
            pub(cell + ".on", !key.empty() && (sx1 == 0 || (x >= sx1 && x + pitch <= sx1 + 1240)) ? 1 : 0);
        }
        const float tt = Ease(epi_tick - epi_tab_at, TabFrames);
        for (int t = 0; t < 3; ++t) {
            const float from = t == epi_tab_from ? 0 : TabDrop, to = t == epi_tab ? 0 : TabDrop;
            pub("epi.ty." + std::to_string(t), std::lround(from + (to - from) * tt));
            text("epi.tab." + std::to_string(t), "epitab/" + title_dir + "/" + std::to_string(t));
        }
        pub("epi.battery", BatteryCharge(host).first >= 0 ? 1 : 0);
        SwapStep(host, epi_bar, EpiBarKey(host, epi_save), "epi.bar", "epi.bar_next", epi_tick, Draw::Urgent);
        text("epi.rbtn.0", "epirbtn/" + title_dir + "/pause");
        text("epi.rbtn.1", "epirbtn/" + title_dir + "/continue");
        // 0 locked, 1 waiting for a full gauge, 2 ready
        const auto state = [&](bool learnt) { return !learnt ? 0 : full ? 2 : 1; };
        text("epi.sp.0", "episp/" + title_dir + "/0/" + std::to_string(state(epi_learnt.cannon)));
        text("epi.sp.1", "episp/" + title_dir + "/1/" + std::to_string(state(epi_learnt.hole)));
        text("epi.sp.2", "episp/" + title_dir + "/2/" + std::to_string(state(epi_learnt.heal)));
        text("epi.sp.3", "episp/" + title_dir + "/3/" + std::to_string(state(epi_learnt.blast)));
        // the gauge: the empty track covers it past its level; a full one glows, its light pulsing in 120 frames
        pub("epi.gauge_on", epi_learnt.cannon ? 1 : 0);
        // in degrees: the segments past it show their track, the edge covers the rest of the one it is in. The
        // HUD's bar moves in steps of about 7 degrees every 6 frames, unseen on its small bar: the ring eases
        // after it a quarter of the way each tick (at least a degree), so it fills smoothly
        const float target = epi_learnt.cannon ? static_cast<float>(shown) : 0.0f;
        if (!live || !epi_settled)
            epi_smooth = target;
        else if (const float d = target - epi_smooth; std::abs(d) <= 1)
            epi_smooth = target;
        else
            epi_smooth += d > 0 ? std::max(1.0f, d / 4) : std::min(-1.0f, d / 4);
        const int level = std::clamp(static_cast<int>(std::lround(epi_smooth)), 0, 360);
        // while it moves the page redraws at 60 Hz (the boost group's animation)
        if (level != epi_level && epi_tick - epi_boost_at > 12)
            epi_boost = !epi_boost, epi_boost_at = epi_tick;
        epi_level = level;
        pub("epi.deg", level);
        pub("epi.edge_on", level % (360 / KirbyModes::GaugeSegments) != 0 && level < 360 ? 1 : 0);
        pub("epi.full", full ? 1 : 0);
        const float wave = full ? 1 - std::abs(static_cast<float>((epi_tick - epi_full_at) % 120) - 60) / 60 : 0;
        pub("epi.glow", std::lround(wave * 15));
    }

    // Pictures every menu page loads through invisible widgets (fix.<n>, and maxpre.<n> on the mode
    // pages), in gen_manifest.py's order, handed over once drawn; the title page's background too.
    void FixedStep(const EdenDsmodHostApi& host) {
        static const std::vector<std::string> fixed = [] {
            std::vector<std::string> keys{"cursor", "dim", "flash"};
            for (int n = 0; n < RainbowSteps; ++n)
                keys.push_back("rainbow/" + std::to_string(n));
            return keys;
        }();
        if (FilesReady("titlebg", true))
            host.publish_text(host.userdata, "title.bg", "module:kirby:gen/files/titlebg");
        if (FilesDone("bgmask"))
            host.publish_text(host.userdata, "bg.mask", "module:kirby:gen/files/bgmask");
        if (FilesDone("bggear"))
            host.publish_text(host.userdata, "bg.gear", "module:kirby:gen/files/bggear");
        // the rainbow's steps are queued by PrewarmFiles, after the file pictures
        for (std::size_t n = 0; n < fixed.size(); ++n)
            if (n < 3 ? FilesReady(fixed[n]) : FilesDone(fixed[n]))
                host.publish_text(host.userdata, ("fix." + std::to_string(n)).c_str(), ("module:kirby:gen/files/" + fixed[n]).c_str());
        if (!max_wanted)
            return;
        for (int n = 0; n < RainbowSteps; ++n) {
            const std::string key = "max/" + std::to_string(n);
            if (FilesDone(key))
                host.publish_text(host.userdata, ("maxpre." + std::to_string(n)).c_str(), ("module:kirby:gen/files/" + key).c_str());
        }
    }
    bool max_wanted{};

    // The DSMod's credits (credits.txt) while the game's staff roll plays, in a game or the Theater: told by its music
    // (App.Application's sound manager [[[[main+0x16324C8]+8]+8]+0xAC8]+0x34B40, BgmPlayer at +0x2DE00, its handle at
    // +0x10 or +0x18 by [player+0x170]: sound id +0x184, frames played +0x180) while a room runs. The cards rise over
    // the starry sky spread over the music, the last stays until the room ends
    static constexpr std::array<std::pair<u32, float>, 3> CreditsMusic{{{0x1000077, 276.0f},   // BGM_NOLP_STAFF1
                                                                       {0x10000D0, 277.5f},   // BGM_NOLP_AKMAHO_STAFFCREDIT
                                                                       {0x10000CF, 272.4f}}}; // BGM_LP_AK_SIMPLECREDIT
    static constexpr u32 CreditCanvas = 1000; // a card's picture: the card at its top, transparent below (gen_manifest CREDIT_CANVAS)
    std::vector<KirbyModes::CreditCard> credit_cards;
    std::vector<float> credit_heights;
    bool credits_read{}, credits_from_sky{};
    int credits_ready_ticks{};
    u64 credits_room{};
    float credits_length{}, credits_t{};

    void CreditsStep(const EdenDsmodHostApi& host, bool game) {
        if (!credits_read) {
            credits_read = true;
            if (const auto text = romfs("file:credits.txt"))
                credit_cards = KirbyModes::ParseCredits({reinterpret_cast<const char*>(text->data()), text->size()});
            credit_heights.assign(credit_cards.size(), 0);
        }
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 room = rd(rd(rd(host.main_base + 0x16325B8) + 0x260) + 0x140);
        const u64 snd = rd(rd(rd(rd(host.main_base + 0x16324C8) + 8) + 8) + 0xAC8), player = snd ? rd(snd + 0x34B40 + 0x2DE00) : 0;
        const u64 handle = player ? rd(player + (static_cast<u32>(rd(player + 0x170)) == 0 ? 0x10 : 0x18)) : 0;
        const u64 music = rd(handle + 0x180);
        const auto it = std::find_if(CreditsMusic.begin(), CreditsMusic.end(), [&](const auto& m) { return m.first == static_cast<u32>(music >> 32); });
        if (credits_room && (!game || room != credits_room))
            credits_room = 0;
        // only from the music's start: it may stay on the handle once played
        if (!credits_room && game && room && it != CreditsMusic.end() && static_cast<u32>(music) < 600 && !credit_cards.empty())
            credits_room = room, credits_length = it->second, credits_t = 0;
        if (credits_room && it != CreditsMusic.end())
            credits_t = std::max(credits_t, static_cast<float>(static_cast<u32>(music)) / 60.0f);
        bool ready = credits_room != 0 && FilesReady("wsky", true);
        for (std::size_t i = 0; i < credit_cards.size() && credits_room; ++i) {
            const std::string key = "credits/" + std::to_string(i);
            if (!FilesReady(key, true)) {
                ready = false;
                continue;
            }
            if (credit_heights[i] == 0)
                if (const auto image = FilesImage(key)) {
                    // the card's own height: its last row with any paint down the middle
                    u32 h = image->height;
                    while (h > 1 && image->rgba[(std::size_t{h - 1} * image->width + image->width / 2) * 4 + 3] == 0)
                        --h;
                    credit_heights[i] = static_cast<float>(h);
                }
        }
        const auto tops = KirbyModes::CreditsTops(credit_heights, credits_t, credits_length);
        if (!credits_room)
            credits_from_sky = false;
        else if (credits_t == 0 && !credits_ready_ticks)
            credits_from_sky = fin_phase == FinSky;
        credits_ready_ticks = ready ? credits_ready_ticks + 1 : 0;
        host.publish_i64(host.userdata, "credits.on", ready ? 1 : 0);
        host.publish_i64(host.userdata, "credits.black", ready && !credits_from_sky ? 1 : 0);
        for (std::size_t i = 0; i < credit_cards.size(); ++i) {
            const bool shown = ready && tops[i] < 1080 && tops[i] + credit_heights[i] > 0;
            host.publish_i64(host.userdata, ("credits.card." + std::to_string(i)).c_str(), shown ? 1 : 0);
            host.publish_i64(host.userdata, ("credits.y." + std::to_string(i)).c_str(), shown ? std::lround(tops[i]) - 1080 : 0);
        }
    }

    // The Epilogue's last blow (user 2026-10-26): told by Magolor's clash scenes, the anim kind on his hero script
    // context [[P+0x130]+0x18] (P = [[[main+0x16325B8]+0x260]+0x140], the player's record; its vtable checked):
    // MstoryTubazeriaiScene2 (pictures drawn ahead), Scene3 the sword's evolution (the gauge fills, then turns every
    // colour with its aura), Scene5 the push the player wins by shaking (the page trembles), then 0x3a7 held still while
    // he cuts the Crown down (a gash carved from the top, then the page's halves fall and the starry sky stays until the
    // credits show). (Its music proved unreliable: one run played none before the cut.)
    static constexpr u32 SwordKind = 0x3E8, EvolveKind = 0x3E9, PushKind = 0x3EB, CutKind = 0x3A7;
    static constexpr std::array<int, 4> FinGashBox{260, 0, 720, 1080}; // gen_manifest.py's FIN_GASH_BOX, FIN_TIP
    static constexpr int FinTip = 260;
    // the offsets on the Epilogue page the shake adds to (gen_manifest.py's fin_shaken: the others take efx.dx / efx.dy)
    static constexpr std::array<const char*, 17> FinShakeX{"bg.dx", "epi.sx", "epim.sx", "epim.a.x", "epim.b.x", "epo.lx", "epo.px",
                                                           "epim.cell.0.x", "epim.cell.1.x", "epim.cell.2.x", "epim.cell.3.x", "epim.cell.4.x",
                                                           "epim.cell.5.x", "epim.cell.6.x", "epim.cell.7.x", "epim.cell.8.x", "epim.cell.9.x"};
    static constexpr std::array<const char*, 4> FinShakeY{"bg.dy", "epi.ty.0", "epi.ty.1", "epi.ty.2"};
    std::optional<KirbyFx::Plan> fin_plan;
    bool fin_plan_read{}, fin_capture{};
    enum FinPhase { FinIdle, FinWait, FinCharge, FinPush, FinCarve, FinFall, FinSky } fin_phase{FinIdle};
    // the cut's timing in ticks: delay before the gash, carving it, the hold, the fall (user:finale.txt overrides them).
    // Magolor comes down on the Crown in 35 frames, eased both ends (StateMasterCrownFinalWin's counter, procMove's
    // HermiteMinToMax): the gash follows him
    std::array<int, 4> fin_times{0, 35, 8, 80};
    u64 fin_tick{}, fin_charge_at{}, fin_cut_at{}, fin_fall_at{}, fin_sky_at{}, fin_room{}, fin_seen_room{};
    int fin_from{}, fin_shot{}, fin_dx{}, fin_dy{};
    u32 fin_kind{~0u};
    std::vector<FxItem> fin_items;
    std::shared_ptr<const KirbyAssets::Image> fin_page;
    int fin_page_id{};
    std::mutex fin_mutex;

    std::shared_ptr<const KirbyAssets::Image> FinPage(int id) {
        std::scoped_lock draw{fx_draw_mutex};
        std::vector<FxItem> items;
        {
            std::scoped_lock lock{fin_mutex};
            if (fin_page_id != id)
                return nullptr;
            if (fin_page)
                return fin_page;
            items = fin_items;
        }
        auto shared = std::make_shared<const KirbyAssets::Image>(DrawItems(items));
        std::scoped_lock lock{fin_mutex};
        if (fin_page_id == id)
            fin_page = shared;
        return shared;
    }

    // efin/gash, efin/tip, efin/<shot>/h/<i> (the page's halves as it was when the cut began)
    std::optional<KirbyAssets::Image> RenderFinale(const std::vector<std::string>& f) {
        if (!fin_plan)
            return std::nullopt;
        const auto& set = fin_plan->sets[0];
        if (f.size() == 2 && f[1] == "gash") {
            const auto sky = FilesImage("wsky");
            return sky ? std::optional{KirbyFx::Gash(*sky, set.cuts[0], FinGashBox)} : std::nullopt;
        }
        if (f.size() == 2 && f[1] == "tip")
            return KirbyFx::Spark(FinTip);
        if (f.size() == 4 && f[2] == "h") {
            const auto page = FinPage(std::atoi(f[1].c_str()));
            const int i = std::atoi(f[3].c_str());
            if (!page || i < 0 || i >= static_cast<int>(set.stages[0].size()))
                return std::nullopt;
            return KirbyFx::CutOut(*page, set.stages[0][static_cast<std::size_t>(i)]);
        }
        return std::nullopt;
    }

    void FinEnd() {
        for (int i = 0; i < 2; ++i)
            FilesForget("efin/" + std::to_string(fin_shot) + "/h/" + std::to_string(i));
        {
            std::scoped_lock lock{fin_mutex};
            fin_items.clear(), fin_page.reset(), fin_page_id = 0;
        }
        fin_phase = FinIdle, fin_dx = fin_dy = 0;
    }

    void FinaleStep(const EdenDsmodHostApi& host, bool on) {
        ++fin_tick;
        if (!fin_plan_read) {
            fin_plan_read = true;
            if (const auto text = romfs("file:finale.txt"))
                fin_plan = KirbyFx::ParsePlan({reinterpret_cast<const char*>(text->data()), text->size()}, false);
            if (host.log && !fin_plan)
                host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, "Kirby DSMod finale: no finale.txt");
        }
        const auto pub = [&](const std::string& n, std::int64_t v) { host.publish_i64(host.userdata, n.c_str(), v); };
        const auto log = [&](const std::string& msg) {
            if (host.log)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod finale: " + msg).c_str());
        };
        if (!on || !fin_plan) {
            if (fin_phase != FinIdle)
                FinEnd();
        }
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        if (on && fin_plan) {
            const u64 player = rd(rd(rd(host.main_base + 0x16325B8) + 0x260) + 0x140), ctx = player ? rd(rd(player + 0x130) + 0x18) : 0;
            const u32 kind = ctx && rd(ctx) == host.main_base + HeroContextVtable ? static_cast<u32>(rd(ctx + 0x12D8 + 0x38)) : ~0u;
            fin_kind = kind;
            if (player != fin_seen_room && player) {
                fin_seen_room = player;
                char buf[64];
                std::snprintf(buf, sizeof buf, "room %llx, Magolor's context %s, kind %x", static_cast<unsigned long long>(player),
                              kind != ~0u ? "linked" : "not linked", kind);
                log(buf);
            }
            if (fin_phase == FinIdle && (kind == SwordKind || kind == EvolveKind)) {
                fin_phase = FinWait, fin_room = player;
                for (const char* key : {"epigauge/rainbow", "epiaura/0", "epiaura/1", "epiaura/2", "efin/gash", "efin/tip"})
                    FilesAhead(key);
                log("the sword's scenes");
            }
            // (out of the room before the cut: over)
            if (fin_phase >= FinWait && fin_phase <= FinPush && player != fin_room)
                log("left before the cut"), FinEnd();
            if (kind == EvolveKind && fin_phase >= FinWait && fin_phase < FinCharge)
                fin_phase = FinCharge, fin_charge_at = fin_tick, fin_from = epi_level, log("charge");
            // the gauge must be seen: back to the Game tab, sliding as a tap would
            if (fin_phase >= FinCharge && fin_phase <= FinCarve && epi_tab != 0)
                epi_tab_from = epi_tab, epi_tab = 0, epi_tab_at = epi_tick, epi_boost = !epi_boost;
            if (kind == PushKind && fin_phase >= FinWait && fin_phase < FinPush) {
                if (fin_phase < FinCharge)
                    fin_charge_at = fin_tick - 120, fin_from = 360;
                fin_phase = FinPush, log("push");
            }
            if (kind == CutKind && fin_phase == FinPush) {
                if (const auto t = romfs("user:finale.txt")) {
                    std::array<int, 4> v{};
                    if (std::sscanf(std::string(t->begin(), t->end()).c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4)
                        fin_times = v;
                }
                fin_capture = true, fin_phase = FinCarve, fin_cut_at = fin_tick, log("cut");
            }
            const auto [delay, carve, hold, fall] = fin_times;
            if (fin_phase == FinCarve && fin_tick - fin_cut_at >= static_cast<u64>(delay + carve + hold))
                fin_phase = FinFall, fin_fall_at = fin_tick, log("fall");
            if (fin_phase == FinFall && fin_tick - fin_fall_at >= static_cast<u64>(fall))
                fin_phase = FinSky, fin_sky_at = fin_tick;
            // the sky gives way to the credits' once they have shown a while (or after two minutes)
            if (fin_phase == FinSky && (credits_ready_ticks >= 30 || fin_tick - fin_sky_at > 7200))
                log("sky done"), FinEnd();
        }
        // the gauge: filling from where it was over a second, then in every colour, its aura pulsing
        const bool gauge = fin_phase >= FinCharge && fin_phase <= FinCarve;
        const float since = static_cast<float>(fin_tick - fin_charge_at);
        if (gauge) {
            const float fill = std::clamp(since / 60, 0.0f, 1.0f), eased = fill * fill * (3 - 2 * fill);
            const int level = std::clamp(static_cast<int>(std::lround(fin_from + (360 - fin_from) * eased)), 0, 360);
            pub("epi.gauge_on", 1), pub("epi.deg", level), pub("epi.full", level >= 360 ? 1 : 0);
            pub("epi.edge_on", level % (360 / KirbyModes::GaugeSegments) != 0 && level < 360 ? 1 : 0);
            pub("epi.glow", level >= 360 ? 15 : 0);
        }
        const bool rain = gauge && since >= 66;
        const float pulse = std::sin(since / 60 * 6.2831853f * 1.4f);
        pub("efx.rain", rain ? 1 : 0);
        pub("efx.ra", rain ? std::min<std::int64_t>(15, static_cast<std::int64_t>(since - 66) * 15 / 24) : 0);
        pub("efx.as0", std::lround(1000 + 40 * pulse)), pub("efx.as1", std::lround(1000 - 40 * pulse));
        // the push: the page trembles (not while a lost push plays out)
        if (fin_phase == FinPush && fin_kind == PushKind) {
            fin_dx = static_cast<int>(fx_random() % 19) - 9, fin_dy = static_cast<int>(fx_random() % 19) - 9;
        } else
            fin_dx = fin_dy = 0;
        pub("efx.dx", fin_dx), pub("efx.dy", fin_dy);
        // the gash, carved from the top as Magolor comes down, its point glowing at its lower end
        const auto [delay, carve, hold, fall] = fin_times;
        const float raw = fin_phase == FinCarve ? std::clamp((static_cast<float>(fin_tick - fin_cut_at) - static_cast<float>(delay)) / static_cast<float>(std::max(1, carve)), 0.0f, 1.0f)
                                                : fin_phase == FinFall ? 1.0f : 0.0f;
        const float p = raw * raw * (3 - 2 * raw);
        const bool gash = fin_phase == FinCarve || (fin_phase == FinFall && fin_tick - fin_fall_at < 8);
        pub("efx.gash", gash ? 1 : 0), pub("efx.carve", std::lround(p * 1000));
        const auto& cut = fin_plan ? fin_plan->sets[0].cuts[0] : KirbyFx::Line{};
        const float ty = p * static_cast<float>(FinGashBox[3]), tx = cut.b.y != cut.a.y ? cut.a.x + (ty - cut.a.y) * (cut.b.x - cut.a.x) / (cut.b.y - cut.a.y) : 0;
        pub("efx.tip", fin_phase == FinCarve && p > 0 && p < 1 ? 1 : 0);
        pub("efx.tip.x", std::lround(tx - FinTip / 2.0f)), pub("efx.tip.y", std::lround(ty - FinTip / 2.0f));
        // the halves fall, leaning out, over the sky
        const float tau = fin_phase == FinFall ? static_cast<float>(fin_tick - fin_fall_at) : 0;
        const float out = std::min(1.0f, tau / static_cast<float>(std::max(1, fall - 2)));
        pub("efx.fall", fin_phase == FinFall ? 1 : 0);
        for (int i = 0; i < 2; ++i) {
            const std::string n = "efx.h." + std::to_string(i), key = "efin/" + std::to_string(fin_shot) + "/h/" + std::to_string(i);
            const float sign = i ? 1.0f : -1.0f;
            pub(n + ".x", std::lround(sign * 60 * out)), pub(n + ".y", std::lround(0.5f * 0.472f * tau * tau)), pub(n + ".r", std::lround(sign * 9 * out));
            if (host.publish_text && fin_phase == FinFall && FilesDone(key))
                host.publish_text(host.userdata, n.c_str(), ("module:kirby:gen/files/" + key).c_str());
        }
        pub("efx.sky", fin_phase >= FinFall ? 1 : 0);
        // the page the halves are cut from, as shown now (after this tick's gauge, in every colour)
        if (fin_capture && fin_plan && host.get_i64 && host.get_text) {
            auto items = CaptureItems(host, fin_plan->page);
            {
                std::scoped_lock lock{fin_mutex};
                fin_items = std::move(items), fin_page.reset(), fin_page_id = ++fin_shot;
            }
            for (int i = 0; i < 2; ++i)
                FilesReady("efin/" + std::to_string(fin_shot) + "/h/" + std::to_string(i), true);
            fin_capture = false;
        }
    }

    // the push's shake on the Epilogue page's own offsets (efx.dx / efx.dy carry it for the rest)
    void FinaleShakeOffsets(const EdenDsmodHostApi& host) {
        if ((fin_dx == 0 && fin_dy == 0) || !host.get_i64)
            return;
        constexpr std::int64_t None = INT64_MIN;
        for (const char* n : FinShakeX)
            if (const auto v = host.get_i64(host.userdata, n, None); v != None)
                host.publish_i64(host.userdata, n, v + fin_dx);
        for (const char* n : FinShakeY)
            if (const auto v = host.get_i64(host.userdata, n, None); v != None)
                host.publish_i64(host.userdata, n, v + fin_dy);
    }

    // Merry Magoland's plaza page (KirbyMago, KirbyGates; the user's sketch 2026-11-04). The plaza runs as the running scene
    // [main+0x16325B8]+0xB0 with first virtual main+0x10DAE8C (the Theater's and the Jukebox's rooms share it, told apart by
    // the main menu's button), without Kirby's usual record. The page stays from the plaza until the game leaves Merry
    // Magoland (the menus, a story room, the world map) and is locked while an attraction runs or loads, on the park's title
    // screen (the same scene) and while a menu covers the plaza: the pause, the missions list, the stamp rally, a dialog, a
    // talk, the mask shop, the players' choice, the controllers (each found by one of its layout's panes, its RootPane
    // shown), an attraction's title menu (told by the ticket's own state, below); until the first search is done, it stays
    // locked.
    static constexpr int MagoTaps = 21; // 0, 1 the arrows; 10 + a door (GateDecoModel's order); 20 the globe
    // (the last is the plaza's own HUD, YmEntranceMain: the page opens only once it has shown, never on the park's title or
    // the loads before it)
    static constexpr std::array<std::string_view, 10> MagoMenus{"L_GuideBtn", "L_EntrancePause", "P_DecoClip", "N_CardShowHide",
                                                                "P_RibbonL",  "T_CharacterNameLong", "L_PlayerPos", "N_PTransN",
                                                                "N_ControllerSelect", "L_MissionClearInfo"};
    static constexpr std::size_t MagoHud = MagoMenus.size() - 1, MagoDialog = 4;
    // A door's tap does what A does in front of it (the user, 2026-11-04). The plaza's script (Basil, ScnYy) shows a door's
    // ticket while Kirby is inside the box in front of it (SubGameDoor/Param's MarkerShowArea<kind>: Bottom, Front, Left,
    // Right, Top in the door's frame, Scn.Yy.Entrance.Info.MarkerIconCtrl), and A on the ticket opens the door's title menu
    // and glides Kirby there (WarpStar.StateWaitMarkerIcon). For a tap the tapped door's box takes in the whole plaza and
    // the others are emptied, the ticket comes up for it wherever Kirby is, the module presses A and puts the boxes back once
    // the ticket has taken it (validated by hand 2026-11-06). Samurai Kirby 100's globe the same way through Info/CheckUpIcon
    // (the globe's reach everywhere, the Grand Prix flag's nil). The kinds are ScnContext.Yy.YySubGameKind's, GateDecoModel's
    // order.
    static constexpr std::array<std::string_view, KirbyGates::Doors> MagoKinds{"EggCatcher", "Hayauchi", "Ochiochi", "Otsukai", "Punch",
                                                                              "Rally",      "Ride",     "Setsuna",  "Shuriken", "Tps"};
    static constexpr int MagoGlobe = KirbyGates::Doors;
    // the tap starts A's own path: the plaza's script (basil/ScnYy.bin, loaded verbatim at a page start) gets the ticket's
    // StateShow without its wait for the show animation and, once the ticket is the tapped door's, StateWaitMarker's test
    // of A (the branch after Hid.IsTrigger(1)) taken; the globe's the same in the icon's StateShowDetailWait; all put back
    // once the game has taken it
    static constexpr u8 YyHeader[32]{0x58, 0x42, 0x49, 0x4e, 0x34, 0x12, 0x02, 0x00, 0x53, 0x2d, 0x22, 0x00, 0xe9, 0xfd, 0x00, 0x00,
                                     0x07, 0x00, 0x08, 0x00, 0x9c, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x97, 0x03, 0x00, 0x00};
    static constexpr StepWord YyShowNow[]{{0x11291c, 0x00530110, 0x005301b6}};
    static constexpr StepWord YyTicketA[]{{0x112a14, 0x00940719, 0x009407b6}};
    static constexpr StepWord YyIconA[]{{0x10fb1c, 0x009f0b19, 0x009f0bb6}};
    // and every trip: a hidden sticker's StateWait takes its used path (its marker inactive), as the markers' most
    // prioritary active one shows (MarkerPrioriryManager) and a sticker near Kirby beats the tapped door
    static constexpr StepWord YyStickersOff[]{{0x9a670, 0x00100919, 0x001009b6}};
    u64 yy_file{};
    bool mago_go_direct{}, mago_go_stickers{};
    // the two boxes files' roots (a Yaml.Data holds one: the file + 0x1C) and Scn.Yy.Entrance.Info.Manager, found through
    // MarkerIconCtrl (its _manager, then _mods, then a Yaml.Data on each file). Basil objects are referenced by their first
    // field; bools pack, an enum takes 8 bytes. The manager holds the ticket's master at +8 and the icons' at +0x10 (their
    // panel at +0x28), the attractions' title menu at +0x50 and the Grand Prix's at +0x60 (IsActive at +1 in both).
    u64 mago_door_yaml{}, mago_icon_yaml{}, mago_manager{};
    std::unordered_set<u64> mago_yaml_other;
    std::vector<std::pair<u64, u32>> mago_undo;
    int mago_go{-1}, mago_go_presses{};
    u64 mago_go_at{}, mago_go_ready{}, mago_go_pressed{};
    std::atomic<int> mago_tap{-1};
    std::array<u64, MagoTaps> mago_pushed{};
    std::array<std::vector<u64>, MagoMenus.size()> mago_roots; // the named panes (their layout's RootPane above them)
    std::vector<u64> mago_hot; // 16 MB places where the plaza's layouts were found, searched again often
    u64 mago_tick{}, mago_cursor{}, mago_hot_cursor{}, mago_since{};
    int mago_section{}, mago_calm{}, mago_passes{};
    bool mago_session{}, mago_on{}, mago_free{}, mago_boost{}, mago_shown{}, mago_hud{};
    Swap mago_count, mago_title, mago_dots;
    std::string mago_covers;
    // the menu panes seen hidden this session: a dialog only counts after that, since the story's own (the world map's
    // question that brings Kirby here) stays shown once it has been used, though it is never drawn again
    std::unordered_set<u64> mago_hidden;

    // the plaza's menus: a slice of the heap a tick (and of the places they were found before), looking for each one's
    // pane name at +0xB0 (NUL after it); a pane's layout root found up its parents (+0x18)
    void MagoMenusScan(const EdenDsmodHostApi& host) {
        const auto named = [&](u64 pane, std::string_view name) {
            char buf[25]{};
            return pane && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0xB0, buf, 24) &&
                   std::string_view{buf} == name;
        };
        for (std::size_t k = 0; k < mago_roots.size(); ++k)
            std::erase_if(mago_roots[k], [&](u64 r) { return !named(r, MagoMenus[k]); });
        for (std::size_t k = 0; k < game_roots.size(); ++k)
            std::erase_if(game_roots[k], [&](u64 r) { return !named(r, GameHuds[k]); });
        const u64 lo = host.get_heap_begin(host.userdata) & ~u64{0xFFF}, hi = host.get_heap_end(host.userdata);
        const auto look = [&](u64 page) {
            const u8* q = host.get_read_pointer(host.userdata, page, 0x1000);
            if (q && !yy_file && std::memcmp(q, YyHeader, sizeof YyHeader) == 0)
                yy_file = page, host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod magoland script " + KirbyHex(page)).c_str());
            for (u32 at = 0; q && at + 8 <= 0x1000; at += 8) {
                u64 v;
                std::memcpy(&v, q + at, 8);
                // (a Yaml.Data on a parameter file: the plaza's are loaded at page starts)
                if ((v & 0xFFF) == 0x1C && v >= lo && v < hi)
                    MagoYamlHolder(host, page + at, v);
                if (at + 25 > 0x1000 || (q[at] != 'L' && q[at] != 'P' && q[at] != 'N' && q[at] != 'T'))
                    continue;
                for (std::size_t k = 0; k < GameHuds.size(); ++k) {
                    const auto name = GameHuds[k];
                    const u64 found = page + at - 0xB0;
                    if (std::memcmp(q + at, name.data(), name.size()) == 0 && q[at + name.size()] == 0 && std::ranges::find(game_roots[k], found) == game_roots[k].end())
                        game_roots[k].push_back(found);
                }
                for (std::size_t k = 0; k < MagoMenus.size(); ++k) {
                    const auto name = MagoMenus[k];
                    if (std::memcmp(q + at, name.data(), name.size()) != 0 || q[at + name.size()] != 0)
                        continue;
                    const u64 found = page + at - 0xB0;
                    u64 pane = found;
                    for (int depth = 0; depth < 12 && pane; ++depth) {
                        if (named(pane, "RootPane")) {
                            if (std::ranges::find(mago_roots[k], found) == mago_roots[k].end()) {
                                mago_roots[k].push_back(found);
                                if (std::ranges::none_of(mago_hot, [&](u64 h) { return found >= h && found < h + 0x1000000; }))
                                    mago_hot.push_back((found - 0x800000) & ~u64{0xFFF});
                            }
                            break;
                        }
                        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0x18, &pane, 8))
                            break;
                    }
                }
            }
        };
        if (mago_cursor < lo)
            mago_cursor = lo;
        // (the first pass faster: the page waits for it)
        for (int n = 0, slice = mago_passes ? 4096 : 16384; n < slice && mago_cursor < hi; ++n, mago_cursor += 0x1000)
            look(mago_cursor);
        if (mago_cursor >= hi)
            mago_cursor = lo, ++mago_passes;
        if (!mago_hot.empty()) {
            const u64 from = mago_hot[static_cast<std::size_t>(mago_tick / 16 % mago_hot.size())];
            for (int n = 0; n < 256; ++n, mago_hot_cursor = (mago_hot_cursor + 0x1000) % 0x1000000)
                look(from + mago_hot_cursor);
        }
    }

    template <typename T>
    bool Rd(const EdenDsmodHostApi& host, u64 at, T* out, std::size_t n = sizeof(T)) const {
        return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, n);
    }

    // HAL's binary YAML: a map {5, n, then n x {key offset from the entry, value offset from its own field}}, a key a u32
    // length and its characters, a float {2, value}
    u64 YamlAt(const EdenDsmodHostApi& host, u64 map, std::string_view key) const {
        u32 head[2]{};
        if (!Rd(host, map, head, 8) || head[0] != 5 || head[1] > 64)
            return 0;
        for (u32 i = 0; i < head[1]; ++i) {
            const u64 at = map + 8 + u64{i} * 8;
            u32 rel[2]{}, len{};
            char name[48]{};
            if (Rd(host, at, rel, 8) && Rd(host, at + rel[0], &len) && len == key.size() && len < sizeof name && Rd(host, at + rel[0] + 4, name, len) &&
                key == std::string_view{name, len})
                return at + 4 + rel[1];
        }
        return 0;
    }

    bool MagoHeapPtr(const EdenDsmodHostApi& host, u64 v) const {
        return (v & 7) == 0 && v >= host.get_heap_begin(host.userdata) && v < host.get_heap_end(host.userdata);
    }

    // EntranceMarker.Panel: IsOperating (A taken, until the ticket waits again), IsFrontAlways, nine objects, _param (on
    // Info/EntranceMarker) at +0x50, the door's kind at +0x58 (11 none) and position, the fiber at +0x70
    bool MagoTicketAt(const EdenDsmodHostApi& host, u64 b) const {
        u8 flags[8]{};
        u64 objs[9]{}, param{}, fiber{};
        u32 kind[2]{};
        return Rd(host, b, flags, 8) && Rd(host, b + 8, objs, sizeof objs) && Rd(host, b + 0x50, &param) && Rd(host, b + 0x58, kind, 8) &&
               Rd(host, b + 0x70, &fiber) && flags[0] <= 1 && flags[1] <= 1 && std::all_of(flags + 2, flags + 8, [](u8 v) { return v == 0; }) &&
               std::ranges::all_of(objs, [&](u64 o) { return MagoHeapPtr(host, o); }) && (param & 0xFFF) == 0x1C && kind[0] <= 11 && kind[1] == 0 &&
               MagoHeapPtr(host, fiber);
    }

    // CheckupIcon.Panel: IsOperating (A taken, until an icon's detail waits again), the icon's kind at +8 (0 the Grand Prix
    // flag, 1 Samurai Kirby 100's globe, 2 a hidden sticker, 3 none), its index, IsPauseState, seven objects, _param at +0x50
    bool MagoIconAt(const EdenDsmodHostApi& host, u64 b) const {
        u8 flags[8]{};
        u32 kind[2]{}, index{};
        u64 objs[7]{}, param{};
        return Rd(host, b, flags, 8) && Rd(host, b + 8, kind, 8) && Rd(host, b + 0x10, &index) && Rd(host, b + 0x18, objs, sizeof objs) &&
               Rd(host, b + 0x50, &param) && flags[0] <= 1 && std::all_of(flags + 1, flags + 8, [](u8 v) { return v == 0; }) && kind[0] <= 3 &&
               kind[1] == 0 && index < 4096 && std::ranges::all_of(objs, [&](u64 o) { return MagoHeapPtr(host, o); }) && param == mago_icon_yaml;
    }

    u64 MagoTicket(const EdenDsmodHostApi& host) const {
        u64 master{}, panel{};
        return Rd(host, mago_manager + 8, &master) && Rd(host, master + 0x28, &panel) && MagoTicketAt(host, panel) ? panel : 0;
    }

    u64 MagoIcon(const EdenDsmodHostApi& host) const {
        u64 master{}, panel{};
        return Rd(host, mago_manager + 0x10, &master) && Rd(host, master + 0x28, &panel) && MagoIconAt(host, panel) ? panel : 0;
    }

    // a Yaml.Data found by the plaza's scan: the two boxes files told by their keys, MarkerIconCtrl by its two fields
    void MagoYamlHolder(const EdenDsmodHostApi& host, u64 holder, u64 root) {
        const auto log = [&](const std::string& m) { host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod magoland " + m).c_str()); };
        if (root != mago_door_yaml && root != mago_icon_yaml) {
            char magic[4]{};
            if (mago_yaml_other.contains(root) || !Rd(host, root - 0x1C, magic, 4) || std::memcmp(magic, "XBIN", 4) != 0)
                return;
            const u64 icons = YamlAt(host, root, "MarkerShowArea");
            if (YamlAt(host, root, "MarkerShowAreaTps") && YamlAt(host, root, "MarkerEndFrame"))
                mago_door_yaml = root, log("door boxes " + KirbyHex(root));
            else if (YamlAt(host, icons, "SetsunaNet") && YamlAt(host, icons, "GrandPrix"))
                mago_icon_yaml = root, log("icon boxes " + KirbyHex(root));
            else if (mago_yaml_other.size() < 4096)
                return void(mago_yaml_other.insert(root));
        }
        u64 door{}, manager{};
        if (root == mago_icon_yaml && !mago_manager && Rd(host, holder - 8, &door) && door == mago_door_yaml && Rd(host, holder - 0x18, &manager)) {
            const u64 was = std::exchange(mago_manager, manager);
            if (MagoTicket(host))
                log("info manager " + KirbyHex(manager));
            else
                mago_manager = was;
        }
    }

    bool MagoYamlLive(const EdenDsmodHostApi& host, u64 root) const {
        char magic[4]{};
        return Rd(host, root - 0x1C, magic, 4) && std::memcmp(magic, "XBIN", 4) == 0;
    }

    // the boxes for a tap: the tapped door's around the whole plaza, the other doors' empty (Left past Right), the icons'
    // reach nil, the globe's everywhere for its own tap; the game's values kept to put back
    bool MagoPatch(const EdenDsmodHostApi& host, int target) {
        std::vector<std::pair<u64, float>> want;
        const auto put = [&](u64 map, std::string_view key, float v) { want.emplace_back(YamlAt(host, map, key), v); };
        for (int k = 0; k < KirbyGates::Doors; ++k) {
            const u64 box = YamlAt(host, mago_door_yaml, "MarkerShowArea" + std::string{MagoKinds[static_cast<std::size_t>(k)]});
            if (k != target)
                put(box, "PosAreaLeft", 1000);
            else
                put(box, "PosAreaLeft", -1000), put(box, "PosAreaRight", 1000), put(box, "PosAreaBottom", -1000), put(box, "PosAreaTop", 1000),
                    put(box, "PosAreaFront", 1000);
        }
        const u64 icons = YamlAt(host, mago_icon_yaml, "MarkerShowArea");
        for (const std::string_view icon : {"GrandPrix", "SetsunaNet"}) {
            const bool on = target == MagoGlobe && icon == "SetsunaNet";
            if (!icons && !on)
                continue; // (the doors without it: a near icon's ticket might win, then the trip gives up)
            const u64 m = YamlAt(host, icons, icon);
            for (const std::string_view key : {"Dist", "DistDetail", "Height", "HeightDetail"})
                if (on || key.starts_with("Dist"))
                    put(m, key, on ? 10000.0f : -1.0f);
        }
        for (const auto& [node, v] : want)
            if (u32 type{}; !Rd(host, node, &type) || type != 2)
                return false;
        for (const auto& [node, v] : want) {
            u32 was{};
            Rd(host, node + 4, &was);
            mago_undo.emplace_back(node + 4, was);
            host.write_memory(host.userdata, node + 4, &v, 4);
        }
        return true;
    }

    void MagoRestore(const EdenDsmodHostApi& host) {
        // (a file freed with the plaza is left alone)
        if (MagoYamlLive(host, mago_door_yaml) && (!mago_icon_yaml || MagoYamlLive(host, mago_icon_yaml)))
            for (auto it = mago_undo.rbegin(); it != mago_undo.rend(); ++it)
                host.write_memory(host.userdata, it->first, &it->second, 4);
        mago_undo.clear();
        if (mago_go_direct)
            for (const auto words : {std::span<const StepWord>{YyShowNow}, std::span<const StepWord>{YyTicketA}, std::span<const StepWord>{YyIconA}})
                BasilPatch(host, yy_file, words, false);
        if (mago_go_stickers)
            BasilPatch(host, yy_file, YyStickersOff, false);
        mago_go_direct = mago_go_stickers = false;
    }

    void MagoGo(const EdenDsmodHostApi& host, int target) {
        const auto log = [&](const std::string& m) { host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod magoland go " + m).c_str()); };
        if (mago_go >= 0 || !host.write_memory || !mago_door_yaml || !mago_manager || (target == MagoGlobe && (!mago_icon_yaml || !MagoIcon(host))))
            return log(std::to_string(target) + ": not ready (boxes " + KirbyHex(mago_door_yaml) + " " + KirbyHex(mago_icon_yaml) + ", manager " +
                       KirbyHex(mago_manager) + ")");
        if (!MagoPatch(host, target)) {
            MagoRestore(host);
            return log(std::to_string(target) + ": the boxes are not as expected");
        }
        mago_go = target, mago_go_at = mago_tick, mago_go_ready = mago_go_pressed = 0, mago_go_presses = 0;
        // (without the script the module presses A instead, once the ticket has settled)
        mago_go_direct = target != MagoGlobe ? BasilPatch(host, yy_file, YyShowNow, true) : yy_file != 0;
        mago_go_stickers = BasilPatch(host, yy_file, YyStickersOff, true);
        log(std::to_string(target) + (mago_go_direct ? " direct" : " with A") + (mago_go_stickers ? "" : ", stickers left on"));
    }

    // a tap's trip: once the ticket (the globe's icon) is the target's, A's path taken at once (or A pressed after the
    // ticket's show animation, again while not taken); everything back once the game has taken it, at once if the player
    // opened something else first (a menu, another door's title menu with their own A), after 1.5 s at most (5 s pressing
    // A). The stick does not stop it: after A the game glides Kirby to the door whatever the stick does, and B on the title
    // menu backs out as usual.
    void MagoGoStep(const EdenDsmodHostApi& host, bool free) {
        if (mago_go < 0)
            return;
        const bool globe = mago_go == MagoGlobe;
        const u64 ticket = MagoTicket(host), icon = MagoIcon(host);
        u8 ticket_taken{}, icon_taken{};
        u32 kind = ~u32{0}, icon_kind = ~u32{0};
        Rd(host, ticket, &ticket_taken), Rd(host, ticket ? ticket + 0x58 : 0, &kind);
        Rd(host, icon, &icon_taken), Rd(host, icon ? icon + 8 : 0, &icon_kind);
        const bool ready = globe ? icon_kind == 1 && !icon_taken : kind == static_cast<u32>(mago_go) && !ticket_taken;
        const bool mine = globe ? icon_kind == 1 && icon_taken : kind == static_cast<u32>(mago_go) && ticket_taken;
        const bool other = globe ? ticket_taken || (icon_taken && icon_kind != 1) : (ticket_taken && !mine) || icon_taken;
        const u64 age = mago_tick - mago_go_at;
        if (mine || other || !free || age > (mago_go_direct ? 90u : 300u) || !ticket) {
            MagoRestore(host);
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                     ("Kirby DSMod magoland go " + std::to_string(mago_go) +
                      (mine ? " taken" : other ? " another taken" : !free ? " stopped by a menu" : " gave up") + " after " + std::to_string(age) + " ticks, " +
                      std::to_string(mago_go_presses) + " A, kind " + std::to_string(static_cast<int>(kind)) + " icon " + std::to_string(static_cast<int>(icon_kind)))
                         .c_str());
            mago_go = -1;
            return;
        }
        if (!ready) {
            mago_go_ready = 0;
            return;
        }
        if (mago_go_direct) {
            if (!mago_go_ready)
                mago_go_ready = mago_tick, BasilPatch(host, yy_file, globe ? std::span<const StepWord>{YyIconA} : std::span<const StepWord>{YyTicketA}, true);
            return;
        }
        if (!mago_go_ready)
            mago_go_ready = mago_tick;
        if (mago_tick - mago_go_ready >= 20 && mago_tick - mago_go_pressed >= 30)
            Press("a"), mago_go_pressed = mago_tick, ++mago_go_presses;
    }

    // The Masks section (KirbyMago, the user's sketch 2026-11-11): the game's Dress-Up Masks buttons in its misaligned rows,
    // Remove first, then Cmn.Hero.OmenKind's order; a tap wears the mask (Remove takes it off), one not got shakes as the
    // game's does (Btn_Rejection). The masks got: the save's u32 per kind at 0x2D10; the one worn: P1's ActorData.OmenKind,
    // the save's at 0x2CF8.
    std::atomic<int> mask_tap{-1};
    std::array<Swap, KirbyMago::MaskSlots> mask_shown;
    int mask_pushed{-1}, mask_shaken{-1};
    u64 mask_pushed_at{}, mask_shaken_at{};
    std::array<bool, KirbyMago::MaskNames.size()> mask_voice{};
    bool mask_voice_known{};

    // which masks speak (their voice mark): OmenDataBase's PosiVoiseSe0..5, any of them a string (OmenUtil.IsExistOmenVoice)
    void MaskVoicesLoad() {
        if (std::exchange(mask_voice_known, true))
            return;
        const auto bytes = romfs("/yaml/Cmn/Omen/OmenDataBase.bin");
        if (!bytes)
            return;
        const auto u = [&](std::size_t at) {
            u32 v{};
            if (at + 4 <= bytes->size())
                std::memcpy(&v, bytes->data() + at, 4);
            return v;
        };
        // HAL's binary YAML in the file (the root map after its 0x1C header): a map's value for `key`, its type first
        const auto get = [&](std::size_t map, std::string_view key) -> std::size_t {
            if (!map || u(map) != 5)
                return 0;
            for (u32 i = 0, n = std::min<u32>(u(map + 4), 512); i < n; ++i) {
                const std::size_t at = map + 8 + std::size_t{i} * 8, k = at + u(at);
                if (u(k) == key.size() && k + 4 + key.size() <= bytes->size() &&
                    std::string_view{reinterpret_cast<const char*>(bytes->data()) + k + 4, key.size()} == key)
                    return at + 4 + u(at + 4);
            }
            return 0;
        };
        const std::size_t base = get(0x1C, "OmenDataBase");
        for (std::size_t k = 0; k < KirbyMago::MaskNames.size(); ++k)
            for (int n = 0; n < 6; ++n)
                if (const std::size_t v = get(get(base, KirbyMago::MaskNames[k]), "PosiVoiseSe" + std::to_string(n)); v && u(v) == 4)
                    mask_voice[k] = true;
    }

    // Wearing a mask without the game's menu (validated by hand 2026-11-11): P1's mask set where the game keeps it (the
    // player's ActorData, the save), then the plaza pause menu's own path for masks run without its menus: a system pause with
    // Pause.MasterCtrl as its callback, OnPause going straight to its ExecSelectOmen and that straight to its end (the gear
    // rebuilt from ActorData, the save asked for, Kirby's mask reaction); both jumps put back once the pause has ended.
    static constexpr StepWord YyMaskWear[]{{0x1495e8, 0xff000593, 0x004dff18}, {0x14980c, 0xff000793, 0x0083ff18}};
    int mask_wear{-1}; // the mask being put on
    u64 mask_wear_at{}, mask_pause{};
    struct MaskChain {
        u64 pause, ctrl, actor;
    };
    // from Info.Manager: its Pause.MasterCtrl (+0xB8) and modules (+0x120); their base modules (+0xA0) with the player
    // manager (+0x18) and GamePause.Manager (+0x40); P1's ActorData, the first of the player manager's array (+0x20, its
    // elements at +0x18); every step checked against the others' back references
    std::optional<MaskChain> MaskObjects(const EdenDsmodHostApi& host) const {
        u64 modules{}, base{}, players{}, pause{}, ctrl{}, array{}, buffer{}, actor{}, back[6]{};
        if (!mago_manager || !Rd(host, mago_manager + 0x120, &modules) || !Rd(host, modules + 0x60, &back[0]) || !Rd(host, modules + 0xA0, &base) ||
            !Rd(host, base + 0x18, &players) || !Rd(host, base + 0x40, &pause) || !Rd(host, mago_manager + 0xB8, &ctrl) || !Rd(host, ctrl, &back[1]) ||
            !Rd(host, pause + 0x20, &back[2]) || !Rd(host, pause + 0x60, &back[3]) || !Rd(host, players + 0x10, &back[4]) ||
            !Rd(host, players + 0x20, &array) || !Rd(host, array + 0x18, &buffer) || !Rd(host, buffer, &actor) || !Rd(host, actor + 0x30, &back[5]))
            return std::nullopt;
        if (back[0] != mago_manager || back[1] != modules || back[2] != base || back[3] != ctrl || back[4] != base || back[5] != base)
            return std::nullopt;
        return MaskChain{pause, ctrl, actor};
    }

    bool MaskWearStart(const EdenDsmodHostApi& host, int kind, u64 data) {
        const auto chain = MaskObjects(host);
        u64 busy[3]{1, 1, 1};
        if (!chain || !host.write_memory || !data)
            return false;
        Rd(host, chain->pause + 0x38, &busy[0]), Rd(host, chain->pause + 0x50, &busy[1]), Rd(host, chain->pause + 0x58, &busy[2]);
        if (busy[0] || busy[1] || busy[2] || !BasilPatch(host, yy_file, YyMaskWear, true))
            return false;
        // the request last: _callbackNext (+0x58), the pad channel (+0x48), _requestKind (+0x38) = 1, a system pause
        const u64 k = static_cast<u64>(kind), one = 1;
        const u32 k32 = static_cast<u32>(kind), channel = 0;
        host.write_memory(host.userdata, chain->actor + 0x18, &k, 8);
        host.write_memory(host.userdata, data + 0x2CF8, &k32, 4);
        host.write_memory(host.userdata, chain->pause + 0x58, &chain->ctrl, 8);
        host.write_memory(host.userdata, chain->pause + 0x48, &channel, 4);
        host.write_memory(host.userdata, chain->pause + 0x38, &one, 8);
        mask_wear = kind, mask_wear_at = mago_tick, mask_pause = chain->pause;
        return true;
    }

    // the pause over once its request, its callback and the next one are all clear again; withdrawn after 2 s untaken (at
    // the plaza's end only the jumps are put back: its objects may be gone)
    void MaskWearStep(const EdenDsmodHostApi& host, bool end) {
        if (mask_wear < 0)
            return;
        u64 state[3]{1, 1, 1};
        Rd(host, mask_pause + 0x38, &state[0]), Rd(host, mask_pause + 0x50, &state[1]), Rd(host, mask_pause + 0x58, &state[2]);
        const bool done = !state[0] && !state[1] && !state[2];
        const u64 age = mago_tick - mask_wear_at;
        if (!done && !end && age < 120)
            return;
        if (!done && state[0] && !end) {
            const u64 zero = 0;
            host.write_memory(host.userdata, mask_pause + 0x38, &zero, 8);
            host.write_memory(host.userdata, mask_pause + 0x58, &zero, 8);
        }
        BasilPatch(host, yy_file, YyMaskWear, false);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                 ("Kirby DSMod magoland mask " + std::to_string(mask_wear) + (done ? " worn" : " withdrawn") + " after " + std::to_string(age) + " ticks").c_str());
        mask_wear = -1;
    }

    std::string MaskKey(const std::string& dir, int slot, bool got, int worn) const {
        if (slot == 0)
            return "mago/mask/r/" + dir + "/" + (worn == 0 ? "1" : "0");
        if (!got)
            return "mago/mask/q/" + dir;
        return "mago/mask/i/" + dir + "/" + std::to_string(slot) + "/" + (mask_voice[static_cast<std::size_t>(slot - 1)] ? "1" : "0") + "/" +
               (worn == slot ? "1" : "0");
    }

    void MagoMasksStep(const EdenDsmodHostApi& host, bool shown, bool free, u64 data, const std::string& dir) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        MaskVoicesLoad();
        MaskWearStep(host, false);
        std::array<u32, KirbyMago::MaskSlots> got{};
        Rd(host, data + 0x2D10, got.data(), sizeof got);
        const auto owned = [&](int slot) { return slot == 0 || got[static_cast<std::size_t>(slot)] > 0; };
        int worn = mask_wear;
        if (worn < 0)
            if (const auto chain = MaskObjects(host)) {
                u64 k{};
                if (Rd(host, chain->actor + 0x18, &k) && k < KirbyMago::MaskSlots)
                    worn = static_cast<int>(k);
            }
        if (worn < 0) {
            u32 k{};
            if (Rd(host, data + 0x2CF8, &k) && k < KirbyMago::MaskSlots)
                worn = static_cast<int>(k);
        }
        if (const int tap = mask_tap.exchange(-1); tap >= 0 && tap < KirbyMago::MaskSlots && shown && free) {
            if (!owned(tap))
                mask_shaken = tap, mask_shaken_at = mago_tick;
            else {
                mask_pushed = tap, mask_pushed_at = mago_tick, mago_boost = !mago_boost;
                if (tap != worn && mask_wear < 0 && mago_go < 0) {
                    if (MaskWearStart(host, tap, data))
                        worn = tap;
                    else
                        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod magoland mask " + std::to_string(tap) + ": not ready").c_str());
                }
            }
        }
        pub("mago.sec2", shown ? 1 : 0);
        for (int i = 0; i < KirbyMago::MaskSlots; ++i) {
            const std::string key = MaskKey(dir, i, owned(i), worn), n = std::to_string(i);
            if (shown)
                SwapStep(host, mask_shown[static_cast<std::size_t>(i)], key, ("mago.mask." + n).c_str(), ("mago.mask." + n + "_next").c_str(), mago_tick,
                         Draw::Urgent);
            else
                FilesAhead(key);
        }
        // Btn_Push: 0.86 at frame 2, back at 6; Btn_Rejection: 4 units right at 1, 3 left at 3, back at 6
        if (const float q = static_cast<float>(mago_tick - mask_pushed_at); shown && mask_pushed >= 0 && q < 6)
            pub("mago.mks." + std::to_string(mask_pushed), std::lround((q < 2 ? 1 - 0.07f * q : 0.86f + 0.035f * (q - 2)) * 1000));
        if (const float q = static_cast<float>(mago_tick - mask_shaken_at); shown && mask_shaken >= 0 && q < 6)
            pub("mago.mkx." + std::to_string(mask_shaken), std::lround((q < 1 ? 4 * q : q < 3 ? 4 - 3.5f * (q - 1) : -3 + (q - 3)) * KirbyMago::MaskScale));
    }

    // The Cards section (KirbyMago, the user's sketch 2026-11-11): the stamp rally's current card as the game's panel shows
    // it, under it the stamps got and the cards finished (15 stamps a card). The save: the card's number u32 at 0x2988 (99 at
    // most), the stamps on it 0x298C, the cards past the 99th 0x2990, the stamps got 0x5A2C.
    Swap cards_card, cards_bar;

    void MagoCardsStep(const EdenDsmodHostApi& host, bool shown, u64 data, const std::string& dir) {
        u32 card[3]{}, total{};
        Rd(host, data + 0x2988, card, sizeof card), Rd(host, data + 0x5A2C, &total);
        const std::string picture = "mago/scard/" + dir + "/" + std::to_string(card[0]) + "/" + std::to_string(static_cast<std::int32_t>(card[2])) + "/" +
                                    std::to_string(card[1]);
        const std::string bar = "mago/sbar/" + std::to_string(total) + "/" + std::to_string(total / 15);
        host.publish_i64(host.userdata, "mago.sec3", shown ? 1 : 0);
        if (!shown) {
            FilesAhead(picture), FilesAhead(bar);
            return;
        }
        SwapStep(host, cards_card, picture, "mago.scard", "mago.scard_next", mago_tick, Draw::Urgent);
        SwapStep(host, cards_bar, bar, "mago.sbar", "mago.sbar_next", mago_tick, Draw::Urgent);
    }

    // The page while an attraction is played (KirbyMago's game page, the user's sketch 2026-11-11): its ticket, the file's
    // pinned missions (a tap shows one's criteria) and a Magoland Tour's four attractions with the one being played framed;
    // up the whole time in the attraction, its menus and results too (the user, 2026-11-12), until the plaza again. The
    // scene's Scn.Yy.BaseModules (Basil) is [[[main+0x1642E60]+0x6E8]+0x110], checked by its player and pause managers' back
    // references; its CreateInfoManager (+0x10) holds the scene's ScnContext.Yy.CreateInfoCommon from +8, its ModeKind at
    // +0xB0: 1 the plaza, 2 an attraction, 4 a tour's round (0 read for a moment at scene changes). A tour's
    // GrandPrixCommonInfo is its GrandPrixDataManager (+0xA0): the attractions' count at +0, their kinds at +0x30 + 8 i (u32,
    // YySubGameKind), the rounds done at +0x50 (validated 2026-11-11 on a level 1 tour). Otherwise the attraction is the one
    // whose title menu opened it (the plaza's Info.Manager +0x50, its _subGameKind at +0x50, validated 2026-11-12), or the one
    // whose InGameBase layout shows (its RootPane, found by a pane only that layout has).
    static constexpr std::array<std::string_view, 11> GameKinds{"EggCatcher", "Hayauchi", "Ochiochi", "Otsukai", "Punch",     "Rally",
                                                                "Ride",       "Setsuna",  "Shuriken", "Tps",     "SetsunaNet"};
    static constexpr std::array<std::string_view, 11> GameHuds{"L_Multi",      "N_ScorePanel", "L_LifeBar01", "N_TimeSRShow", "L_Gauge_Timer", "L_ReadyStart",
                                                               "L_TimeCenter", "L_CharaNameL", "L_Target",    "N_HPGaugeSet", "L_Count"};
    std::array<std::vector<u64>, GameKinds.size()> game_roots;
    struct GameScene {
        int mode{};
        std::array<int, 4> tour{-1, -1, -1, -1};
        int round{-1};
    };
    GameScene game_scene, game_tour_last;
    int game_kind{-1}, game_entry{-1}, game_sel{}, game_pushed{-1};
    u64 game_pushed_at{};
    std::atomic<int> game_tap{-1};
    std::array<Swap, PinSlots> game_rows;
    std::array<Swap, 4> game_cards;
    Swap game_card, game_ticket, game_pins_box, game_tour_box;

    GameScene GameSceneNow(const EdenDsmodHostApi& host) const {
        GameScene scene;
        u64 a{}, b{}, base{}, players{}, pause{}, back[2]{}, info{}, grand{};
        if (!Rd(host, host.main_base + 0x1642E60, &a) || !Rd(host, a + 0x6E8, &b) || !Rd(host, b + 0x110, &base) || !Rd(host, base + 0x18, &players) ||
            !Rd(host, players + 0x10, &back[0]) || !Rd(host, base + 0x40, &pause) || !Rd(host, pause + 0x20, &back[1]) || back[0] != base ||
            back[1] != base || !Rd(host, base + 0x10, &info))
            return scene;
        u32 mode{}, count{}, round{};
        Rd(host, info + 0xB0, &mode);
        scene.mode = static_cast<int>(mode);
        if (mode == 4 && Rd(host, base + 0xA0, &grand) && grand && Rd(host, grand, &count) && Rd(host, grand + 0x50, &round) && count == 4 && round < 4) {
            for (int i = 0; i < 4; ++i)
                if (u32 k{}; Rd(host, grand + 0x30 + 8 * static_cast<u64>(i), &k) && k < 10)
                    scene.tour[static_cast<std::size_t>(i)] = static_cast<int>(k);
            scene.round = static_cast<int>(round);
        }
        return scene;
    }

    // a layout pane's RootPane shown (flags 0x81, not see-through), the chain reaching it
    bool GameHudShown(const EdenDsmodHostApi& host, u64 pane) const {
        for (int depth = 0; depth < 12 && pane; ++depth) {
            u8 flags[2]{};
            char name[25]{};
            if (!Rd(host, pane + 0x58, flags, 2) || !Rd(host, pane + 0xB0, name, 24))
                return false;
            if (std::string_view{name} == "RootPane")
                return (flags[0] & 0x81) == 0x81 && flags[1];
            if (!Rd(host, pane + 0x18, &pane))
                return false;
        }
        return false;
    }

    void GameStep(const EdenDsmodHostApi& host, u64 data, const std::string& dir) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        // the attraction: a tour's round's, the one whose HUD shows, the one already shown, the one whose title menu opened
        const int current = game_scene.round >= 0 ? game_scene.tour[static_cast<std::size_t>(game_scene.round)] : -1;
        int kind = current;
        for (int k = 0; k < static_cast<int>(GameKinds.size()) && kind < 0 && game_scene.mode >= 2; ++k)
            if (std::ranges::any_of(game_roots[static_cast<std::size_t>(k)], [&](u64 r) { return GameHudShown(host, r); }))
                kind = k;
        if (kind < 0)
            kind = game_kind >= 0 ? game_kind : game_entry;
        const bool on = (game_scene.mode >= 2 || (game_scene.mode == 0 && game_kind >= 0)) && kind >= 0;
        if (on != (game_kind >= 0) || (on && kind != game_kind))
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                     ("Kirby DSMod magoland attraction " + (on ? std::string{GameKinds[static_cast<std::size_t>(kind)]} : std::string{"left"}) + " (mode " +
                      std::to_string(game_scene.mode) + ", round " + std::to_string(game_scene.round) + ")")
                         .c_str());
        game_kind = on ? kind : -1;
        // the file's pins in the missions list's order, a done one left out
        MagoMissionsLoad();
        const int file = files_cursor <= 2 ? static_cast<int>(files_cursor) : -1;
        std::vector<std::pair<std::pair<int, int>, int>> pins;
        u32 phases[120]{};
        Rd(host, data + 0x2B04, phases, sizeof phases);
        if (file >= 0 && mis_pins_known[static_cast<std::size_t>(file)] && mago_missions_ok)
            for (const int save : mis_pins[static_cast<std::size_t>(file)])
                if (const auto at = save >= 0 && save < 120 && !phases[save] ? MagoMissionAt(save) : std::nullopt)
                    pins.emplace_back(*at, save);
        std::ranges::sort(pins);
        const int rows = static_cast<int>(pins.size());
        if (const int tap = game_tap.exchange(-1); tap >= 0 && tap < rows && on)
            game_sel = tap, game_pushed = tap, game_pushed_at = mago_tick;
        game_sel = std::clamp(game_sel, 0, std::max(rows - 1, 0));
        const bool tour = current >= 0;
        const std::string box = "game/box/" + dir + "/", ticket = "game/ticket/" + dir + "/" + std::to_string(std::max(kind, 0));
        pub("game.on", on ? 1 : 0);
        pub("game.tour.on", tour ? 1 : 0);
        pub("game.card.on", rows ? 1 : 0);
        for (int i = 0; i < PinSlots; ++i)
            pub("game.row.on." + std::to_string(i), i < rows ? 1 : 0);
        if (!on)
            return;
        SwapStep(host, game_ticket, ticket, "game.ticket", "game.ticket_next", mago_tick, Draw::Urgent);
        SwapStep(host, game_pins_box, box + "0/" + (rows ? "0" : "1"), "game.pinsbox", "game.pinsbox_next", mago_tick, Draw::Urgent);
        SwapStep(host, game_tour_box, box + "1/" + (tour ? "0" : "1"), "game.tourbox", "game.tourbox_next", mago_tick, Draw::Urgent);
        host.publish_text(host.userdata, "game.bg", "module:kirby:gen/files/mago/bg");
        for (int i = 0; i < rows; ++i)
            SwapStep(host, game_rows[static_cast<std::size_t>(i)],
                     "mago/mrow/" + dir + "/" + std::to_string(pins[static_cast<std::size_t>(i)].second) + "/" + (i == game_sel ? "1" : "0") + "/0",
                     ("game.row." + std::to_string(i)).c_str(), ("game.row." + std::to_string(i) + "_next").c_str(), mago_tick, Draw::Urgent);
        if (rows)
            SwapStep(host, game_card, "mago/mcard/" + dir + "/" + std::to_string(pins[static_cast<std::size_t>(game_sel)].second) + "/0", "game.card",
                     "game.card_next", mago_tick, Draw::Urgent);
        // Btn_Push on the row tapped: 0.86 at frame 2, back at 6
        if (const float q = static_cast<float>(mago_tick - game_pushed_at); game_pushed >= 0 && q < 6)
            pub("game.rs." + std::to_string(game_pushed), std::lround((q < 2 ? 1 - 0.07f * q : 0.86f + 0.035f * (q - 2)) * 1000));
        if (!tour)
            return;
        for (int i = 0; i < 4; ++i)
            SwapStep(host, game_cards[static_cast<std::size_t>(i)], "game/tcard/" + dir + "/" + std::to_string(std::max(game_scene.tour[static_cast<std::size_t>(i)], 0)),
                     ("game.tcard." + std::to_string(i)).c_str(), ("game.tcard." + std::to_string(i) + "_next").c_str(), mago_tick, Draw::Urgent);
        host.publish_text(host.userdata, "game.tarrow", "module:kirby:gen/files/game/tarrow");
        host.publish_text(host.userdata, "game.tcursor", "module:kirby:gen/files/game/tcursor");
        pub("game.cursor.x", game_scene.round * KirbyMago::TourPitch);
    }

    // The Missions section (KirbyMago, the user's sketch 2026-11-09). The rows sit in the manifest's scroll region
    // "mago.list" (its rows 1 px tall, so its follow_bind sets an exact offset: the scrollbar's drag), the scrollbar's
    // track reports its drags (on_pan). Pins: at most five a file, a pinned mission first in its set, none on a done one.
    static constexpr float MisTop = 372, MisView = 708, MisFirst = 6, MisPitch = 88;
    int mis_set{};
    std::array<int, KirbyMago::SetCount> mis_sel{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<std::array<int, PinSlots>, 3> mis_pins{}; // save indices, -1 none, as the user set them
    std::array<bool, 3> mis_pins_known{};
    std::array<u64, MagoRows> mis_row_pushed{};
    u64 mis_shake_at{};
    std::atomic<int> mis_row{-1}, mis_pin{-1}, mis_slide{0}, mis_pan{0};
    // the list's offset: the module scrolls it (a drag on the list, on the scrollbar, a fling after a list drag), as the
    // module runs before the runtime scrolls and so could not read @scroll itself
    float mis_offset{}, mis_velocity{};
    int mis_offset_set{-1};
    u64 mis_pan_tick{};
    std::array<Swap, MagoRows> mis_rows_shown;
    Swap mis_card, mis_bar, mis_pin_count, mis_dots, mis_clear;
    std::int64_t mis_follow{-1}, mis_shown_offset{};

    int MisStored(int file, int slot) const {
        int v = 0;
        for (int b = 0; b < PinBits; ++b)
            v |= (options[static_cast<std::size_t>(PinBit + PinSlots * PinBits * file + PinBits * slot + b)] ? 1 : 0) << b;
        return v - 1;
    }

    void MagoMissionsStep(const EdenDsmodHostApi& host, bool shown, bool free, bool extra, const u32 (&phases)[120], const std::string& dir) {
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        MagoMissionsLoad();
        const int file = files_cursor <= 2 ? static_cast<int>(files_cursor) : -1;
        const int row_tap = mis_row.exchange(-1), pin_tap = mis_pin.exchange(-1), slide = mis_slide.exchange(0);
        if (!mago_missions_ok || file < 0) {
            pub("mago.sec1", 0);
            return;
        }
        auto& pins = mis_pins[static_cast<std::size_t>(file)];
        // the file's pins once the flags have been handed over; a done mission loses its pin
        if (!mis_pins_known[static_cast<std::size_t>(file)] && options_known) {
            for (int k = 0; k < PinSlots; ++k)
                pins[static_cast<std::size_t>(k)] = MisStored(file, k);
            mis_pins_known[static_cast<std::size_t>(file)] = true;
        }
        const auto done = [&](int save) { return save >= 0 && save < MagoMissionCount && phases[save] > 0; };
        for (int& p : pins)
            if (p >= MagoMissionCount || done(p))
                p = -1;
        std::stable_partition(pins.begin(), pins.end(), [](int p) { return p >= 0; });
        const auto pinned = [&](int save) { return std::ranges::find(pins, save) != pins.end(); };
        const int count = static_cast<int>(std::ranges::count_if(pins, [](int p) { return p >= 0; }));
        // stored once they are known
        if (mis_pins_known[static_cast<std::size_t>(file)])
            for (int k = 0; k < PinSlots; ++k)
                for (int b = 0; b < PinBits; ++b) {
                    const int i = PinBit + PinSlots * PinBits * file + PinBits * k + b, want = ((pins[static_cast<std::size_t>(k)] + 1) >> b) & 1;
                    if (options[static_cast<std::size_t>(i)] != want)
                        Store(i, want);
                }
        const int sets = extra ? KirbyMago::SetCount : KirbyMago::SetCount - 1;
        mis_set = std::clamp(mis_set, 0, sets - 1);
        const auto& set = mago_missions[static_cast<std::size_t>(mis_set)];
        // the rows: pinned first, each part in the game's order
        std::vector<int> order;
        for (const auto& m : set)
            if (pinned(m.save))
                order.push_back(m.save);
        for (const auto& m : set)
            if (!pinned(m.save))
                order.push_back(m.save);
        const int rows = std::min(static_cast<int>(order.size()), MagoRows);
        int& sel = mis_sel[static_cast<std::size_t>(mis_set)];
        if (std::ranges::find(order, sel) == order.end())
            sel = order.empty() ? -1 : order.front();
        // (a tap on the list while it glides only stops it, as the runtime's lists do)
        const bool caught = (row_tap >= 0 || pin_tap >= 0) && std::abs(mis_velocity) > 2;
        if (caught)
            mis_velocity = 0;
        if (shown && free && !caught) {
            if (row_tap >= 0 && row_tap < rows)
                sel = order[static_cast<std::size_t>(row_tap)], mis_row_pushed[static_cast<std::size_t>(row_tap)] = mago_tick;
            if (pin_tap >= 0 && pin_tap < rows && !done(order[static_cast<std::size_t>(pin_tap)])) {
                const int save = order[static_cast<std::size_t>(pin_tap)];
                if (pinned(save))
                    *std::ranges::find(pins, save) = -1, std::stable_partition(pins.begin(), pins.end(), [](int p) { return p >= 0; });
                else if (count < PinSlots)
                    *std::ranges::find(pins, -1) = save;
                else
                    mis_shake_at = mago_tick;
            }
        }
        // the scroll: a list drag moves it with the finger (then glides, slowing as the runtime's lists do), a scrollbar drag
        // moves the thumb with the finger (the region's own thumb: the view's share of the list, 24 px at least), so the list
        // by as much more as its range is longer than the thumb's; each new offset set through follow_bind
        const float content = MisFirst + MisPitch * static_cast<float>(rows) + 8, most = std::max(0.0f, content - MisView);
        const float thumb = std::clamp(MisView * MisView / content, 24.0f, MisView), travel = MisView - thumb;
        const int pan = mis_pan.exchange(0);
        if (mis_offset_set != mis_set)
            mis_offset = 0, mis_velocity = 0, mis_offset_set = mis_set; // (the region's reset_bind has put it back too)
        if (shown && free && (pan != 0 || slide != 0)) {
            // a new drag takes over from a glide at once; the glide's speed is the drag's last few ticks'
            if (mago_tick - mis_pan_tick > 2)
                mis_velocity = 0;
            mis_offset -= static_cast<float>(pan);
            if (travel > 0)
                mis_offset += static_cast<float>(slide) * most / travel;
            mis_velocity = slide != 0 ? 0 : mis_velocity * 0.5f - static_cast<float>(pan) * 0.5f, mis_pan_tick = mago_tick;
        } else if (mago_tick - mis_pan_tick > 2 && std::abs(mis_velocity) > 0.5f)
            mis_offset += mis_velocity, mis_velocity *= 0.92f;
        else if (mago_tick - mis_pan_tick > 2)
            mis_velocity = 0;
        if (mis_offset <= 0 || mis_offset >= most)
            mis_offset = std::clamp(mis_offset, 0.0f, most), mis_velocity = mago_tick - mis_pan_tick > 2 ? 0 : mis_velocity;
        pub("mago.list.on", shown && col.phase == 0 ? 1 : 0);
        if (const float at = std::round(mis_offset); mis_follow < 0 || std::abs(at - static_cast<float>(mis_shown_offset)) >= 1) {
            if (mis_follow >= 0 || at > 0)
                mis_follow = static_cast<std::int64_t>(at < static_cast<float>(mis_shown_offset) ? at : at + MisView - 1);
            mis_shown_offset = static_cast<std::int64_t>(at);
        }
        pub("mago.list.h", static_cast<std::int64_t>(content));
        pub("mago.list.follow", mis_follow);
        pub("mago.list.reset", mis_set);
        pub("mago.rows", rows);
        const auto text = [&](const std::string& name, const std::string& key, bool urgent) {
            if (FilesReady(key, urgent))
                host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + key).c_str());
        };
        pub("mago.sec1", shown ? 1 : 0);
        if (!shown) {
            // ahead: the set's rows and card, for a quick switch to the section
            for (int i = 0; i < rows; ++i)
                FilesAhead("mago/mrow/" + dir + "/" + std::to_string(order[static_cast<std::size_t>(i)]) + "/" + (order[static_cast<std::size_t>(i)] == sel ? "1" : "0") +
                           "/" + (done(order[static_cast<std::size_t>(i)]) ? "1" : "0"));
            return;
        }
        for (const char* k : {"mago/marrow/0", "mago/marrow/1", "mago/mpin/0", "mago/mpin/1", "mago/mthumb", "mago/mtrack"})
            FilesReady(k, true);
        text("mago.marrow.0", "mago/marrow/0", true), text("mago.marrow.1", "mago/marrow/1", true);
        int set_done = 0;
        for (const auto& m : set)
            set_done += done(m.save);
        SwapStep(host, mis_bar, "mago/mset/" + dir + "/" + std::to_string(mis_set), "mago.mset", "mago.mset_next", mago_tick, Draw::Urgent);
        SwapStep(host, mis_pin_count, "mago/mpins/" + std::to_string(count), "mago.mpins", "mago.mpins_next", mago_tick, Draw::Urgent);
        SwapStep(host, mis_dots, "mago/mdots/" + std::to_string(sets) + "/" + std::to_string(mis_set), "mago.mdots", "mago.mdots_next", mago_tick, Draw::Urgent);
        SwapStep(host, mis_clear, "mago/mclear/" + std::to_string(set_done) + "/" + std::to_string(set.size()), "mago.mclear", "mago.mclear_next", mago_tick,
                 Draw::Urgent);
        if (sel >= 0)
            SwapStep(host, mis_card, "mago/mcard/" + dir + "/" + std::to_string(sel) + "/" + (done(sel) ? "1" : "0"), "mago.mcard", "mago.mcard_next", mago_tick,
                     Draw::Urgent);
        // the counter shakes when a sixth pin is refused (a short decaying swing)
        const float q = static_cast<float>(mago_tick - mis_shake_at);
        pub("mago.mpins.dx", mis_shake_at && q < 24 ? std::lround(7 * std::sin(q * 0.9f) * (1 - q / 24)) : 0);
        for (int i = 0; i < MagoRows; ++i) {
            const auto ix = static_cast<std::size_t>(i);
            if (i >= rows) {
                pub("mago.pin.on." + std::to_string(i), 0);
                continue;
            }
            const int save = order[ix];
            const std::string n = std::to_string(i);
            SwapStep(host, mis_rows_shown[ix], "mago/mrow/" + dir + "/" + std::to_string(save) + "/" + (save == sel ? "1" : "0") + "/" + (done(save) ? "1" : "0"),
                     ("mago.row." + n).c_str(), ("mago.row." + n + "_next").c_str(), mago_tick, Draw::Urgent);
            pub("mago.pin.on." + n, done(save) ? 0 : 1);
            pub("mago.pinx." + n, save == sel ? 33 : 0);
            text("mago.pin." + n, pinned(save) ? "mago/mpin/1" : "mago/mpin/0", true);
            float scale = 1;
            if (const float k = static_cast<float>(mago_tick - mis_row_pushed[ix]); mis_row_pushed[ix] && k < 8)
                scale = k < 3 ? 1 - 0.04f * k / 3 : 0.96f + 0.04f * (k - 3) / 5;
            pub("mago.rs." + n, std::lround(scale * 1000));
        }
        // the neighbour sets' bars, and every row's other look, ahead
        for (const int d : {sets - 1, 1})
            FilesAhead("mago/mset/" + dir + "/" + std::to_string((mis_set + d) % sets));
        for (int i = 0; i < rows; ++i) {
            const int save = order[static_cast<std::size_t>(i)];
            FilesAhead("mago/mrow/" + dir + "/" + std::to_string(save) + "/" + (save == sel ? "0" : "1") + "/" + (done(save) ? "1" : "0"));
        }
        for (int i = 0; i < rows; ++i) {
            const int save = order[static_cast<std::size_t>(i)];
            FilesAhead("mago/mcard/" + dir + "/" + std::to_string(save) + "/" + (done(save) ? "1" : "0"));
        }
    }

    void MagolandStep(const EdenDsmodHostApi& host, bool menu) {
        ++mago_tick;
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const auto pub = [&](const std::string& name, std::int64_t v) { host.publish_i64(host.userdata, name.c_str(), v); };
        const u64 root = rd(host.main_base + 0x16325B8), slot = rd(rd(root + 0xB0)), room = rd(rd(root + 0x260) + 0x140);
        const bool plaza = !menu && slot == host.main_base + 0x10DAE8C && menu_button != 3 && menu_button != 4 && !past_adventures;
        if (plaza)
            mago_session = true;
        if (menu || room || wmap_live || files_cursor > 2)
            mago_session = false;
        const u64 data = mago_session ? SlotData(host, static_cast<int>(files_cursor), true) : 0;
        if (!mago_session || !data || title_dir.empty() || !host.publish_text) {
            if (mago_on)
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, "Kirby DSMod magoland off");
            if (!mago_undo.empty())
                MagoRestore(host);
            MaskWearStep(host, true);
            mago_on = mago_free = mago_shown = mago_hud = false, mago_tap = -1, mago_calm = 0, mago_go = -1, mask_tap = -1, game_kind = -1, game_tap = -1;
            pub("mago.on", 0), pub("mago.can", 0), pub("game.on", 0);
            return;
        }
        if (!mago_on) {
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, "Kirby DSMod magoland on"), mago_passes = 0, mago_cursor = 0, mago_since = mago_tick;
            mago_door_yaml = mago_icon_yaml = mago_manager = 0, mago_yaml_other.clear(), mago_hidden.clear();
            for (auto& roots : game_roots)
                roots.clear();
        }
        mago_on = true;
        // between a tour's rounds (the next attraction's ticket) its info is gone for a moment: the page keeps the last tour
        // until the next round's comes (the cursor then moves on) or the plaza
        game_scene = GameSceneNow(host);
        if (game_scene.round >= 0 || game_scene.mode == 1 || game_kind < 0)
            game_tour_last = game_scene.round >= 0 ? game_scene : GameScene{};
        else if (game_tour_last.round >= 0)
            game_scene.tour = game_tour_last.tour, game_scene.round = game_tour_last.round;
        if (plaza)
            MagoMenusScan(host);
        // a menu shows while its layout's RootPane has flags 0x81 (0x80 once closed, the plaza's other loaded layouts 0x01) and
        // is not see-through (validated 2026-11-05 headless: the players' choice and the pause)
        std::string covers, chains;
        for (std::size_t k = 0; k < mago_roots.size(); ++k)
            for (const u64 r : mago_roots[k]) {
                bool shown = false, rooted = false; // (only a chain that reaches its RootPane counts: freed panes keep their names)
                std::string chain = " " + std::string{MagoMenus[k]} + "@" + KirbyHex(r) + "[";
                u64 pane = r;
                for (int depth = 0; depth < 12 && pane; ++depth) {
                    u8 flags[2]{};
                    float scale[2]{};
                    if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0x58, flags, 2) ||
                        !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0x48, scale, 8))
                        break;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%s%x/%x/%.2f", depth ? " " : "", flags[0], flags[1], scale[1]);
                    chain += buf;
                    char name[25]{};
                    if (dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0xB0, name, 24) && std::string_view{name} == "RootPane") {
                        shown = (flags[0] & 0x81) == 0x81 && flags[1], rooted = true;
                        break;
                    }
                    if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, pane + 0x18, &pane, 8))
                        pane = 0;
                }
                if (rooted && !shown)
                    mago_hidden.insert(r);
                const bool stale = shown && k == MagoDialog && !mago_hidden.contains(r);
                chains += (stale ? " ?" + chain.substr(1) : chain) + "]";
                if (shown && k == MagoHud)
                    mago_hud = true;
                else if (shown && !stale)
                    covers += " " + std::string{MagoMenus[k]};
            }
        for (u64* root : {&mago_door_yaml, &mago_icon_yaml})
            if (*root && !MagoYamlLive(host, *root))
                *root = 0;
        if (const u8* q = yy_file ? host.get_read_pointer(host.userdata, yy_file, sizeof YyHeader) : nullptr; !q || std::memcmp(q, YyHeader, sizeof YyHeader) != 0)
            yy_file = 0;
        if (mago_manager && !MagoTicket(host))
            mago_manager = 0;
        // an attraction's title menu (the globe's too) or the Grand Prix's; a door's ticket taken by A (from the press until
        // the ticket waits again, after its title menu has gone)
        // (the icon's A stays taken after a hidden sticker's, which has nothing to come back to)
        u8 ticket_taken{}, icon_taken{};
        u32 icon_kind{};
        const u64 ticket = mago_manager ? MagoTicket(host) : 0, icon = mago_manager ? MagoIcon(host) : 0;
        Rd(host, ticket, &ticket_taken), Rd(host, icon, &icon_taken), Rd(host, icon ? icon + 8 : 0, &icon_kind);
        if (ticket_taken)
            covers += " ticket";
        if (icon_taken && icon_kind < 2)
            covers += " icon";
        const bool covered = !covers.empty();
        if (covers.find(MagoMenus[0]) != std::string::npos)
            mago_hud = false; // the park's title again
        // research: the menus shown and every found menu pane's chain (flags/alpha/scale y, from the pane up), when they change
        if (covers != mago_covers)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod magoland menus:" + (covered ? covers : std::string{" none"}) + " |" + chains).c_str());
        mago_covers = covers;
        // a menu locks the page at once; it opens again once the plaza has been clear for a moment (the game takes a few
        // frames between two menus: the park's title and the players' choice), and not before the plaza's script is up
        // (back from the story the last visit's HUD is still there, shown, through the load; 15 s at most without it)
        const bool up = mago_manager || mago_tick - mago_since > 900;
        // (and only in the plaza's own scene: an attraction's keeps the plaza's HUD shown behind it, a scene change reads none)
        mago_calm = plaza && !covered && mago_passes > 0 && mago_hud && up && game_scene.mode == 1 ? mago_calm + 1 : 0;
        const bool free = mago_calm >= 40;
        if (free != mago_free)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, free ? "Kirby DSMod magoland free" : "Kirby DSMod magoland locked");
        mago_free = free;
        // the missions done: the first hundred (the game's count to 100), then all 120 once the final ones are open
        u32 missions[120]{};
        dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, data + 0x2B04, missions, sizeof missions);
        const auto per_set = MagoDone(missions);
        const int finals = per_set.back(), first = std::accumulate(per_set.begin(), per_set.end() - 1, 0);
        const bool all = first == 100 || finals > 0;
        // Samurai Kirby 100 opens once Samurai Kirby has been played (CheckUpIcon.Utility.CheckUnlockCondition: SetsunaMission1
        // done, the first of Samurai Kirby's set)
        const bool globe = mago_missions_ok && missions[mago_missions[1].front().save] > 0;
        if (const int tap = mago_tap.exchange(-1);
            tap >= 0 && tap < MagoTaps && free && (tap <= 1 || (tap <= 3 ? mago_section == 1 : mago_section == 0))) {
            mago_pushed[static_cast<std::size_t>(tap)] = mago_tick, mago_boost = !mago_boost;
            if (tap <= 1)
                mago_section = (mago_section + (tap == 1 ? 1 : KirbyMago::Sections - 1)) % KirbyMago::Sections;
            else if (tap == 2 || tap == 3) {
                const int sets = all ? KirbyMago::SetCount : KirbyMago::SetCount - 1;
                mis_set = (mis_set + (tap == 3 ? 1 : sets - 1)) % sets;
            } else if (tap >= 10 && tap < 10 + KirbyGates::Doors)
                MagoGo(host, tap - 10);
            else if (tap == 20 && globe)
                MagoGo(host, MagoGlobe);
        }
        MagoGoStep(host, free);
        MagoMissionsStep(host, mago_section == 1 && mago_shown, free, all, missions, title_dir);
        MagoMasksStep(host, mago_section == 2 && mago_shown, free, data, title_dir);
        MagoCardsStep(host, mago_section == 3 && mago_shown, data, title_dir);
        if (u64 menu{}; mago_manager && game_scene.mode == 1 && Rd(host, mago_manager + 0x50, &menu) && menu)
            if (u32 k{}; Rd(host, menu + 0x50, &k) && k < GameKinds.size())
                game_entry = static_cast<int>(k);
        GameStep(host, data, title_dir);
        const std::string sec = std::to_string(mago_section);
        const auto text = [&](const std::string& name, const std::string& key) {
            host.publish_text(host.userdata, name.c_str(), ("module:kirby:gen/files/" + key).c_str());
        };
        // the page comes up whole: its fixed pictures and the shown section's
        std::vector<std::string> fixed{"mago/bg", "mago/arrow/0", "mago/arrow/1"};
        for (int i = 0; i < KirbyGates::Doors; ++i)
            fixed.push_back("mago/door/" + std::to_string(i));
        bool ready = true;
        for (const auto& key : fixed)
            ready = FilesReady(key, !mago_shown) && ready;
        if (globe)
            ready = FilesReady("mago/globe", !mago_shown) && ready;
        for (int i = 0; i < KirbyMago::Sections; ++i)
            FilesAhead("mago/title/" + title_dir + "/" + std::to_string(i)), FilesAhead("mago/dots/" + std::to_string(i));
        SwapStep(host, mago_title, "mago/title/" + title_dir + "/" + sec, "mago.title", "mago.title_next", mago_tick, Draw::Urgent);
        SwapStep(host, mago_dots, "mago/dots/" + sec, "mago.dots", "mago.dots_next", mago_tick, Draw::Urgent);
        SwapStep(host, mago_count, "mago/count/" + title_dir + "/" + std::to_string(all ? first + finals : first) + "/" + (all ? "120" : "100"),
                 "mago.count", "mago.count_next", mago_tick, Draw::Urgent);
        ready = ready && !mago_title.shown.empty() && !mago_dots.shown.empty() && !mago_count.shown.empty();
        mago_shown |= ready;
        for (const auto& key : fixed)
            if (FilesDone(key)) {
                // mago/door/3 -> mago.door.3
                std::string bind = key;
                std::replace(bind.begin(), bind.end(), '/', '.');
                text(bind, key);
            }
        if (FilesDone("mago/globe"))
            text("mago.globe", "mago/globe");
        // ModeSelectBtn*_Push: down to 0.92 and back in 8 frames
        for (int i = 0; i < MagoTaps; ++i) {
            if (i >= 4 && i < 10)
                continue;
            float scale = 1;
            if (const float q = static_cast<float>(mago_tick - mago_pushed[static_cast<std::size_t>(i)]); mago_tick >= mago_pushed[static_cast<std::size_t>(i)] && q < 8)
                scale = q < 3 ? 1 - 0.08f * q / 3 : 0.92f + 0.08f * (q - 3) / 5;
            pub("mago.scale." + std::to_string(i), std::lround(scale * 1000));
        }
        pub("mago.sec0", mago_section == 0 ? 1 : 0);
        pub("mago.globe_on", mago_section == 0 && globe && FilesDone("mago/globe") ? 1 : 0);
        pub("mago.lock", free ? 0 : 1);
        pub("mago.can", free && mago_go < 0 && mask_wear < 0 ? 1 : 0);
        pub("mago.boost", mago_boost ? 1 : 0);
        pub("mago.on", mago_shown ? 1 : 0);
    }

    void TitleStep(const EdenDsmodHostApi& host) {
        FixedStep(host);
        const auto rd32 = [&](u64 at) {
            u32 v{};
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 4);
            return v;
        };
        const auto menu = TitleMenu(host);
        const bool movie = rd32(host.main_base + 0x1626C30) == 1;
        // 0 not in the title menu, 1 attract (demo or movie), 2 title, 3 file select, 4 mode select.
        // A game is only entered from the mode select and the attract movie only starts from the
        // title, so the last menu screen tells which one runs once the menu is gone (main+0x1626C30
        // is 1 on the world map too).
        if (!menu && menu_screen != 0)
            from_game = menu_screen == 4, menu_screen = 0;
        int screen = movie && !from_game ? 1 : 0;
        bool ready = false;
        std::int64_t scale = 1000, alpha = 15;
        if (menu) {
            // M+0x13A0 stays 3 after the first trip back to the title, so the file select is told by
            // its own phase and the title from the mode select by the start button still showing.
            const u32 state = rd32(*menu + 0x4D0), sub = rd32(*menu + 0x13A0), files = rd32(*menu + 0x1EC8);
            // State 0 with M+0x13A0 at 0 is both the title coming back after the attract movie and the
            // mode select (or the main mode's menu) reopened from a game: told apart by what ran before
            if (state != 0 || files != 0)
                from_game = false;
            screen = files != 0 ? 3 : state == 1 ? 2 : state == 0 && sub != 2 && !from_game ? 1 : 4;
            u64 layout{};
            dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *menu + 0xFC0, &layout, 8);
            // N_Start's own show / hide is mirrored; the push only plays for a tap on this screen
            if (const auto start = TitlePane(host, layout, 0)) {
                float sx{};
                u8 a{};
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *start + 0x48, &sx, 4);
                dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *start + 0x59, &a, 1);
                const float push = title_frame - title_push < 10 ? Curve(StartPush, static_cast<float>(title_frame - title_push)) : 1;
                scale = std::clamp<std::int64_t>(std::lround(sx * push * 1000), 0, 2000);
                alpha = (a * 15 + 127) / 255;
                // only a menu still in its first run (state 2) shows the title again this way; one
                // rebuilt after a game (state 0) keeps the hidden button's alpha
                if (screen == 4 && a != 0 && state != 0)
                    screen = 2;
                ready = screen == 2 && a == 255;
            }
        }
        ++title_frame;
        if (title_tap.exchange(false) && ready) {
            title_push = title_frame;
            Press("a");
            title_boost = !title_boost;
        }
        host.publish_i64(host.userdata, "title.boost", title_boost ? 1 : 0);
        if (menu)
            menu_screen = screen;
        // the mode select is only reached from the file select or a game: the few frames between the
        // title and the file select that look like it do not count
        if (screen == 3 || from_game)
            modes_ok = true;
        else if (screen == 1 || screen == 2)
            modes_ok = false;
        if (screen != 0)
            title_grace = 90;
        else if (title_grace > 0 && !from_game)
            --title_grace, screen = 1;
        FilesStep(host, menu, screen);
        WorldMapStep(host, menu.has_value());
        // Magolor Epilogue has a page of its own (for now the background in its colours)
        const bool epilogue = !menu && menu_button == 2;
        arena_mode = menu ? 0 : menu_button == 1 ? 1 : menu_button == 8 ? 2 : 0;
        // (no tabs there: the controls are the Game tab's)
        if (arena_mode)
            play_tab = tab_from = 0;
        PlayStep(host, screen == 0 && !wmap_live && !epilogue);
        MagolandStep(host, menu.has_value());
        host.publish_i64(host.userdata, "arena.on", arena_mode ? 1 : 0);
        if (!arena_mode)
            arena_board.clear(), arena_character = "Kirby", arena_rest = false, vs_state = VsNone, arena_doomer_fights = 0, arena_doomers.clear(),
                arena_shown.clear(), arena_figure.clear(), arena_time = -1;
        if (!arena_mode && soul_on)
            SoulReset();
        if (arena_mode && host.publish_text && !title_dir.empty())
            ArenaStep(host);
        SprayStep(host);
        ColourMenuStep(host);
        ColourModelStep(host);
        SmokeStep(host);
        YyColourStep(host);
        host.publish_i64(host.userdata, "epi.on", epilogue ? 1 : 0);
        if (epilogue && host.publish_text && !title_dir.empty())
            EpilogueStep(host);
        else {
            epi_started = epi_live = epi_settled = epi_save_ok = epi_learnt_ok = false, epi_free = 0, epi_loaded = epi_tick;
            if (epi_pop)
                PopupSeen();
            // out of the Epilogue mid-Heal: the scripts as the game shipped them
            if (heal_phase != 0)
                heal_phase = 0, HealPatch(host, false);
            if (blast_phase != 0)
                blast_phase = 0, StepPatch(host, BlastPatches, false), HeroAnimNames(host, BlastNames, false), BlastDamage(host, false);
        }
        // the background's colours: story, extra, Magolor Epilogue, Arena, True Arena (kept between rooms too)
        const int colours = epilogue ? 2 : arena_mode ? 2 + arena_mode : play_mode;
        // its icons fade to white while the game loads (until the next room is up; in the Epilogue while its
        // page waits), one of 16 steps a tick, and back
        const bool white = epilogue ? epi_started && !epi_live : play_between;
        play_white = std::clamp(play_white + (white ? 1 : -1), 0, 15);
        host.publish_i64(host.userdata, "play.tint", colours * 16 + play_white);
        for (int k = 0; k < 5; ++k)
            host.publish_i64(host.userdata, ("play.bg." + std::to_string(k)).c_str(), k == colours ? 1 : 0);
        // (after the page's ground, which the cut page's picture draws too)
        if (host.publish_text)
            FxStep(host, screen == 0 && !wmap_live && !epilogue && !arena_mode && files_cursor <= 2);
        // a film in a game (a story cutscene, the Theater's) darkens the game's pages with the logo, as the attract demo
        // does: main+0x1626C30 is 1 while it plays and the running scene ([main+0x16325B8]+0xB0) is a film's (first
        // virtual main+0x9881E8; main+0x986A44 builds it for the story's films and the Theater's). The flag is up in other
        // scenes without a room too: the world map (main+0x10C522C) while it loads, the Theater's room (main+0x10DAE8C),
        // the Arenas' title (main+0x9324AC). In the Theater a cutscene made in the game's engine (a stage's scene,
        // main+0x1025150) counts too
        const auto rd = [&](u64 at) {
            u64 v{};
            return at && dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, &v, 8) ? v : 0;
        };
        const u64 scene = rd(rd(rd(host.main_base + 0x16325B8) + 0xB0));
        const bool film_scene = scene == host.main_base + 0x9881E8;
        const bool theater = !menu && menu_button == 3 && (film_scene || scene == host.main_base + 0x1025150);
        // so is the ship part's throw, from its room through the fade-out until the film that follows takes over
        const bool was_ship = ship_cover;
        if (play_ship >= 0)
            ship_cover = play_ship == 1;
        if ((movie && film_scene) || screen != 0 || !from_game || wmap_live)
            ship_cover = false;
        if (ship_cover != was_ship)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, ship_cover ? "Kirby DSMod ship cover on" : "Kirby DSMod ship cover off");
        const bool film = screen == 0 && from_game && ((movie && film_scene) || theater || ship_cover);
        host.publish_i64(host.userdata, "play.movie", film ? 1 : 0);
        // Past Adventures, the Jukebox and the Theater have a page with a closed curtain, darkened by the Theater's films;
        // (down while one plays, so the page comes back after it)
        const bool curtain = (past_adventures || (!menu && (menu_button == 3 || menu_button == 4))) && !film;
        host.publish_i64(host.userdata, "curtain.on", curtain && FilesReady("curtain", true) ? 1 : 0);
        FinaleStep(host, screen == 0 && from_game && epilogue);
        CreditsStep(host, screen == 0 && from_game);
        host.publish_i64(host.userdata, "title.screen", screen);
        host.publish_i64(host.userdata, "title.on", screen != 0 ? 1 : 0);
        host.publish_i64(host.userdata, "title.attract", screen == 1 ? 1 : 0);
        host.publish_i64(host.userdata, "title.ready", ready ? 1 : 0);
        host.publish_i64(host.userdata, "title.btn_scale", scale);
        host.publish_i64(host.userdata, "title.btn_alpha", screen == 1 ? 15 : alpha);
        if (const auto dir = GameMessageDir(host); !dir.empty() && dir != title_dir) {
            KirbyFont::SetRegion(dir);
            title_dir = dir;
        }
        if (host.publish_text && !title_dir.empty()) {
            for (const auto& [name, key] : {std::pair<const char*, std::string>{"title.btn", "startbtn/" + title_dir},
                                            std::pair<const char*, std::string>{"title.logo", "logo/" + title_dir}})
                if (FilesReady(key, true))
                    host.publish_text(host.userdata, name, ("module:kirby:gen/files/" + key).c_str());
        }
    }

    // Player record X = [[main+0x16325B8]+0x260]: Kirby P at X+0x140 (trusted only while its
    // copy-ability component [P+0x220] points back to it) and the souvenir-use state S at X+0x9B8,
    // where S+0xFC is the hold threshold (40) and S+0x100 the frames X has been held for it.
    static constexpr int UseWindow = 30; // frames to wait for the press to reach the game

    std::optional<u64> UseState(const EdenDsmodHostApi& host) const {
        using dsmod_sdk::MissingIsMapped;
        u64 root{}, record{}, player{}, comp{}, owner{}, state{};
        const auto read = [&](u64 at, u64& out) {
            return dsmod_sdk::ReadGuest<MissingIsMapped::Reject>(host, at, &out, 8);
        };
        if (!read(host.main_base + 0x16325B8, root) || !read(root + 0x260, record) ||
            !read(record + 0x140, player) || !read(player + 0x220, comp) || !read(comp, owner) ||
            owner != player || !read(record + 0x9B8, state))
            return std::nullopt;
        return state;
    }

    void UseReserveStep(const EdenDsmodHostApi& host) {
        const bool start = use_requested.exchange(false);
        if (start)
            use_frames = UseWindow;
        if (start)
            Press("x");
        if (use_frames == 0)
            return;
        --use_frames;
        const auto state = UseState(host);
        u32 hold[2]{}; // threshold, held frames
        if (!state || !host.write_memory ||
            !dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *state + 0xFC, hold, sizeof hold))
            return;
        // the press has reached the game (counter running) and the item is not out yet
        if (hold[0] > 2 && hold[0] < 600 && hold[1] >= 1 && hold[1] < hold[0] - 1) {
            const u32 boosted = hold[0] - 1;
            host.write_memory(host.userdata, *state + 0x100, &boosted, 4);
            if (host.log) {
                const std::string msg = "Kirby DSMod use_reserve: hold " + std::to_string(hold[1]) + " -> " +
                                        std::to_string(boosted) + " of " + std::to_string(hold[0]) + " after " +
                                        std::to_string(UseWindow - use_frames) + " frame(s)";
                host.log(host.userdata, EDEN_DSMOD_LOG_INFO, msg.c_str());
            }
            use_frames = 0;
        }
    }

    // decode_font: the package's font file holds "<font>[:<cap px>]".
    EdenDsmodBool DecodeFont(const u8* bytes, std::size_t size, void* receiver, EdenDsmodFontSink sink) {
        if (!bytes || !sink)
            return EDEN_DSMOD_FALSE;
        const auto spec = KirbyFont::ParseFontSpec({bytes, size});
        const auto font = spec ? KirbyFont::BuildFontAtlas(romfs, *spec, true) : nullptr;
        if (!font)
            return EDEN_DSMOD_FALSE;
        sink(receiver, font->line_height, font->first_codepoint, font->glyphs.data(),
             static_cast<std::uint32_t>(font->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    }
};

EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id && std::string_view{build_id} == BuildId ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_romfs)
            return nullptr;
        return new Module{*host};
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    delete static_cast<Module*>(p);
}

void Sample(void* p, const EdenDsmodHostApi* host) {
    if (!host || !host->publish_i64)
        return;
    auto* m = static_cast<Module*>(p);
    if (m) {
        // the stage page's background (PauseBg_Wait's, its lattice 1.3 times wider: BG_SLIDE in
        // gen_manifest.py): the icons slide one lattice step, right and down, per 240-frame loop
        if (!m->options[0] && !m->fx_frozen && !m->soul_frozen && m->col.phase == 0)
            m->bg_frame = (m->bg_frame + 1) % 240;
        host->publish_i64(host->userdata, "bg.dx", (689 * m->bg_frame + 120) / 240);
        host->publish_i64(host->userdata, "bg.dy", (494 * m->bg_frame + 120) / 240);
        // and every icon turns a full circle anticlockwise per loop
        host->publish_i64(host->userdata, "bg.r", -static_cast<std::int64_t>(m->bg_frame * 3 / 2));
        m->UseReserveStep(*host);
        m->TitleStep(*host);
        m->PublishPress(*host);
        m->SoulCaptureNow(*host);
    }
    if (m && m->scan_requested.exchange(false))
        m->ScanPlayer(*host);
    if (m && m->ptrscan_requested.exchange(false))
        m->PointerScan(*host);
    if (m && m->statescan_requested.exchange(false))
        m->StateScan(*host);
    if (m && m->bytefind_requested.exchange(false))
        m->ByteFind(*host);
    if (m && m->findseq_requested.exchange(false))
        m->FindSequence(*host);
    if (m && m->basefind_requested.exchange(false))
        m->BaseFind(*host);
    if (m && m->vscan_requested.exchange(false))
        m->ValueScan(*host);
    if (m && m->hscan_requested.exchange(false))
        m->HeapScan(*host);
    if (m && m->layoutscan_requested.exchange(false))
        m->LayoutScan(*host);
    if (m && m->vtscan_requested.exchange(false))
        m->VtableScan(*host);
    if (m && m->vscan_track > 0)
        m->TrackStep(*host);
    if (m && m->trace_toggle.exchange(false)) {
        m->tracing = !m->tracing, m->trace_learnt = 0, m->trace_state.clear();
        for (auto& t : m->trace)
            t = {t.name};
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, m->tracing ? "Kirby DSMod trace on" : "Kirby DSMod trace off");
    }
    if (m && m->tracing)
        m->TraceStep(*host);
    if (m && m->render_requested.exchange(false))
        m->RenderDump(*host);
    if (m && m->objfind_requested.exchange(false))
        m->ObjFind(*host);
    if (m && m->panewatch_requested.exchange(false))
        m->PaneFind(*host);
    if (m)
        m->PaneWatch(*host);
    if (m && m->bgmwatch_toggle.exchange(false))
        m->bgmwatch = !m->bgmwatch;
    if (m && m->bgmwatch)
        m->BgmWatch(*host);
    if (m && m->fxwatch_toggle.exchange(false)) {
        m->fxwatch = !m->fxwatch, m->fxw_room = 0;
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, m->fxwatch ? "Kirby DSMod fxwatch on" : "Kirby DSMod fxwatch off");
    }
    if (m && m->fxwatch)
        m->FxWatch(*host);
    if (m && m->star_requested.exchange(false)) {
        if (const u64 data = m->files_cursor <= 2 ? m->SlotData(*host, static_cast<int>(m->files_cursor), true) : 0) {
            const u32 star = 1;
            host->write_memory(host->userdata, data - 0x28 + 0x2D30, &star, 4);
            m->star_wait = 3;
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "Kirby DSMod abilitystar: reserve set");
        }
    }
    if (const int kind = m ? m->newhunt_requested.exchange(0) : 0) {
        // newforce reads "<vtable main offset hex> <field offset hex>" from user:newforce.txt
        m->force_vt = m->force_off = 0;
        if (kind == 2)
            if (const auto t = m->romfs("user:newforce.txt")) {
                unsigned long long vt{}, off{};
                if (std::sscanf(std::string(t->begin(), t->end()).c_str(), "%llx %llx", &vt, &off) == 2)
                    m->force_vt = host->main_base + vt, m->force_off = off;
            }
        m->before = m->VtablesNow(*host);
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, ("Kirby DSMod newhunt start, " + std::to_string(m->before.size()) + " vtables").c_str());
        m->hunt_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        m->hunt_delay = 1, m->hunt_phase = 3, m->hunt_ticks = 0, m->hunt_wait = 0, m->hunt_posed = false;
    }
    if (m && m->hunt_requested.exchange(false)) {
        m->HeapOp(*host, "init", 0, "starhunt init");
        // after the long scan the runtime catches up on ticks at once: wait in real time
        m->hunt_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        m->hunt_delay = 1;
        m->hunt_phase = 1, m->hunt_ticks = 0, m->hunt_wait = 0, m->hunt_posed = false;
    }
    if (m && m->hunt_delay > 0 && std::chrono::steady_clock::now() >= m->hunt_at)
        m->hunt_delay = 0, m->star_requested = true;
    if (m && m->hunt_phase > 0 && m->hunt_delay == 0)
        m->HuntStep(*host);
    if (m && m->star_wait > 0 && --m->star_wait == 0)
        m->use_requested = true;
    if (m)
        m->FxShakeOffsets(*host), m->FinaleShakeOffsets(*host);
    // supports_build only accepts 1.0.0, so a running instance always matches.
    host->publish_i64(host->userdata, "build_match", 1);
    host->publish_i64(host->userdata, "kirby.module", 1);
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void*, const EdenDsmodHostExtensions*) {}

EdenDsmodBool OnAction(void* p, const char* action, std::int64_t argument) {
    if (!p || !action)
        return EDEN_DSMOD_FALSE;
    auto* m = static_cast<Module*>(p);
    const std::string_view name{action};
    if (name == "use_reserve") {
        m->use_requested = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "play_resume") {
        m->resume_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "epi_tap") {
        m->epi_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "epi_resume") {
        m->epi_resume_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "spray_pop") {
        m->spray_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "epi_pop") {
        m->epi_pop_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "play_tap") {
        m->play_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "col_tap") {
        m->col_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    // a drag's step: dx in the payload's high half, dy in its low half
    if (name == "col_pan" || name == "col_thumb") {
        (name == "col_pan" ? m->col_pan : m->col_thumb) += static_cast<std::int32_t>(static_cast<std::uint32_t>(argument));
        return EDEN_DSMOD_TRUE;
    }
    if (name == "soul_tap") {
        m->soul_tap_pending = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "soul_scream") { // research
        m->soul_force_scream = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "opt" && argument >= 0 && argument < Module::Options * 2) {
        m->options[static_cast<std::size_t>(argument / 2)] = static_cast<int>(argument % 2);
        m->options_known = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "opt_tap") {
        m->opt.tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "abl_tap") {
        m->abl.tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "epo_tap") {
        m->epo.tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "epim_tap") {
        m->epim.tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "mago_tap") {
        m->mago_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "mago_mask") {
        m->mask_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "game_row") {
        m->game_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "mago_row" || name == "mago_pin") {
        (name == "mago_row" ? m->mis_row : m->mis_pin) = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "mago_slide") { // a pan: dx in the high word, dy in the low one
        m->mis_slide += static_cast<std::int32_t>(static_cast<std::uint32_t>(argument));
        return EDEN_DSMOD_TRUE;
    }
    if (name == "mago_pan") {
        m->mis_pan += static_cast<std::int32_t>(static_cast<std::uint32_t>(argument));
        return EDEN_DSMOD_TRUE;
    }
    if (name == "wmap_tap") {
        m->wmap_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "menu_tap") {
        m->menu_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "title_start") {
        m->title_tap = true;
        return EDEN_DSMOD_TRUE;
    }
    if (name == "file_tap" && argument >= 0 && argument <= 2) {
        m->file_tap = static_cast<int>(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (name == "scan_player")
        m->scan_requested = true;
    else if (name == "bytefind")
        m->bytefind_requested = true;
    else if (name == "statescan")
        m->statescan_requested = true;
    else if (name == "ptrscan")
        m->ptrscan_requested = true;
    else if (name == "findseq")
        m->findseq_requested = true;
    else if (name == "basefind")
        m->basefind_requested = true;
    else if (name == "vscan")
        m->vscan_requested = true;
    else if (name == "vtscan")
        m->vtscan_requested = true;
    else if (name == "layoutscan")
        m->layoutscan_requested = true;
    else if (name == "hscan")
        m->hscan_requested = true;
    else if (name == "trace")
        m->trace_toggle = true;
    else if (name == "render")
        m->render_requested = true;
    else if (name == "objfind")
        m->objfind_requested = true;
    else if (name == "panewatch")
        m->panewatch_requested = true;
    else if (name == "bgmwatch")
        m->bgmwatch_toggle = true;
    else if (name == "fxwatch")
        m->fxwatch_toggle = true;
    else if (name == "abilitystar")
        m->star_requested = true;
    else if (name == "starhunt")
        m->hunt_requested = true;
    else if (name == "newhunt" || name == "newforce")
        m->newhunt_requested = name == "newhunt" ? 1 : 2;
    else
        return EDEN_DSMOD_FALSE;
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (!p)
            return EDEN_DSMOD_FALSE;
        auto* m = static_cast<Module*>(p);
        // research: what the host asks for, whether the workers had it drawn, and how long the host waited
        constexpr std::string_view Files{"module:kirby:gen/files/"};
        const std::string_view k = key ? std::string_view{key} : std::string_view{};
        const bool files = k.starts_with(Files), ready = files && m->FilesDone(std::string{k.substr(Files.size())});
        const auto started = std::chrono::steady_clock::now();
        const auto loaded = m->LoadImage(host, key, receiver, sink);
        if (host && host->log) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            const std::string note = files ? (ready ? "ready" : "drawn on the host's thread") : "made on the host's thread";
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                      ("Kirby DSMod host load " + std::string{k.substr(std::min(k.size(), std::string_view{"module:kirby:"}.size()))} + " " +
                       std::to_string(ms) + " ms, " + note + (loaded ? "" : ", FAILED"))
                          .c_str());
        }
        return loaded;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Kirby's Return to Dream Land Deluxe DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &Sample,
                                   &Tick};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &Configure,             &OnAction,                         &LoadImageCallback};
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, std::size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
