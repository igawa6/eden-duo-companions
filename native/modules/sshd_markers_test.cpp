// SPDX-License-Identifier: GPL-3.0-or-later
// Skyward Sword HD map markers (the two map marker analyses): Link's view cone,
// the Sky's Goddess tags (cube struck AND chest closed), opened ordinary chests, and (dev.27) the
// live-object statues / door bars / neighbour plates and the Sky layers, all fail closed.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sshd_art.h"
#include "sshd_plate.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54;
struct State {
    std::string stage;
    int16_t yaw = 0;
    uint32_t scene_name = 0x6C616544;   // anything but the title's 'tleS'
    std::map<uint64_t, uint32_t> words; // u32 reads by address
    std::map<uint64_t, std::vector<uint8_t>> blocks; // guest memory (object tree, save flags)
    uint8_t story[0x200]{};             // story words (the mirror main+0x182E1F8 = the file's words here)
    bool readable = true;
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> texts;
    std::vector<std::string> romfs;       // SSHD_ROMFS roots (LiveFit)
    std::map<std::string, double> views;  // "@map_view_*" answers (LiveFit)
};


// The native story getter's chain (sshd_story.h) over the test's story block: manager
// main+0x182E1F0 -> +0x18 -> +8 = the decoded mapping table, save manager main+0x2BF9CC0 so
// that its file's words (+0x10 + 0x8E4) are the story block at 0x182E1F8.
#include "sshd_story_table.inc"
bool StoryChain(uint64_t addr, void* out, size_t n) {
    constexpr uint64_t kMgr = 0x7000000, kMeta = 0x7001000, kTable = 0x7002000;
    const auto ptr = [&](uint64_t v) {
        if (n != 8)
            return false;
        std::memcpy(out, &v, 8);
        return true;
    };
    if (addr == 0x182E1F0)
        return ptr(kMgr);
    if (addr == kMgr + 0x18)
        return ptr(kMeta);
    if (addr == kMeta + 8)
        return ptr(kTable);
    if (addr == 0x2BF9CC0)
        return ptr(0x182E1F8 - 0x10 - 0x8E4);
    if (addr >= kTable && addr + n <= kTable + sizeof(kFxStoryTable)) {
        std::memcpy(out, kFxStoryTable + (addr - kTable), n);
        return true;
    }
    return false;
}

EdenDsmodBool Read(void* p, uint64_t addr, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    if (!s.readable)
        return EDEN_DSMOD_FALSE;
    std::memset(out, 0, n);
    if (StoryChain(addr, out, n))
        return EDEN_DSMOD_TRUE;
    for (const auto& [a, b] : s.blocks)
        if (addr >= a && addr + n <= a + b.size()) {
            std::memcpy(out, b.data() + (addr - a), n);
            return EDEN_DSMOD_TRUE;
        }
    if (addr == kSlot + 0x531C) {
        std::memcpy(out, s.stage.data(), std::min(n, s.stage.size()));
    } else if (addr == 0x623E7BE && n == 2) {
        std::memcpy(out, &s.yaw, 2);
    } else if (addr == 0x182A7F4 && n == 4) {
        std::memcpy(out, &s.scene_name, 4);
    } else if (addr >= 0x182E1F8 && addr + n <= 0x182E1F8 + sizeof(s.story)) {
        std::memcpy(out, s.story + (addr - 0x182E1F8), n);
    } else if (n == 4 && s.words.count(addr)) {
        std::memcpy(out, &s.words[addr], 4);
    }
    return EDEN_DSMOD_TRUE; // every other read is zero (an empty save region)
}

// --- dev.28 label fit (sshd_plate.h): neighbour plates / island names stay fully inside the map
// viewport and clear of the zones drawn over it, moving as little as they must, or hide.
constexpr sshd::Box kView{20, 20, 1220, 904};          // Map page viewport
constexpr sshd::Box kCompass{1065, 21, 1215, 171};     // compass
constexpr sshd::Box kReset{1004, 778, 1220, 906};      // Reset plank (dev.38; the Zoom plank before; dev.41 128 tall)
constexpr sshd::Box kTitle{32, 26, 677, 146};          // Sealed Grounds title plate

sshd::Box BoxAt(std::array<float, 2> c, float w, float h) {
    return {c[0] - w / 2, c[1] - h / 2, c[0] + w / 2, c[1] + h / 2};
}

// distance from a point to a box (0 inside)
float Gap(std::array<float, 2> p, const sshd::Box& b) {
    return std::hypot(std::max({b.x0 - p[0], 0.0f, p[0] - b.x1}), std::max({b.y0 - p[1], 0.0f, p[1] - b.y1}));
}

bool Clear(const sshd::Box& b, std::span<const sshd::Box> zones) {
    for (const auto& z : zones)
        if (sshd::Overlaps(b, z))
            return false;
    return true;
}

void LabelFit() {
    const sshd::Box zones[] = {kCompass, kReset, kTitle};
    // the native placement: the box edge meets the mark along the direction (cw from up)
    {
        const auto r = sshd::PlaceOffset(90, 200, 360, 50);
        assert(std::fabs(r[0] - 120.0f) < 0.01f && std::fabs(r[1]) < 0.01f); // tw * 1.2 / 2 right
        const auto u = sshd::PlaceOffset(0, 200, 360, 50);
        assert(std::fabs(u[0]) < 0.01f && std::fabs(u[1] + 25.0f) < 0.01f); // half the band up
        const auto c = sshd::PlaceOffset(90, 600, 1080, 50); // kept on the 900 px art canvas
        assert(std::fabs(c[0] - (900.0f - 540.0f - 450.0f)) < 0.01f);
    }
    // a native spot that fits is kept exactly
    {
        const auto r = sshd::FitLabel({600, 500}, {700, 470}, 360, 50, kView, zones, 240);
        assert(r && (*r)[0] == 700.0f && (*r)[1] == 470.0f);
    }
    // capture 04: the Faron Woods plate past the right edge and under the compass. The fit lies
    // inside the viewport, clear of every zone, and within the allowed gap of its mark.
    {
        const std::array<float, 2> mark{1060, 150}, native{1060 + 113, 150 - 25};
        const float w = 364, h = 54;
        assert(!sshd::Within(BoxAt(native, w, h), kView) && !Clear(BoxAt(native, w, h), zones)); // the bug
        const auto r = sshd::FitLabel(mark, native, w, h, kView, zones, 120);
        assert(r);
        const sshd::Box b = BoxAt(*r, w, h);
        assert(sshd::Within(b, kView) && Clear(b, zones));
        assert(Gap(mark, b) <= 120.01f);
    }
    // no zones: the box is clamped onto the viewport edge nearest its anchor (x only here)
    {
        const auto r = sshd::FitLabel({40, 500}, {-40, 500}, 200, 50, kView, {}, 240);
        assert(r && std::fabs((*r)[0] - 120.0f) < 0.01f && (*r)[1] == 500.0f);
    }
    // hidden: a box larger than the viewport, a mark too far outside (no box within the gap), no
    // way past a zone
    {
        assert(!sshd::FitLabel({600, 500}, {600, 500}, 1300, 50, kView, zones, 240));
        assert(!sshd::FitLabel({3000, 3000}, {3100, 3000}, 200, 50, kView, zones, 240));
        const sshd::Box wall[] = {{20, 20, 1220, 480}, {20, 520, 1220, 904}}; // a 40 px gap only
        assert(!sshd::FitLabel({600, 500}, {600, 500}, 200, 50, kView, wall, 240));
        // a 60 px gap fits the 50 px band exactly between the two zones
        const sshd::Box gap[] = {{20, 20, 1220, 470}, {20, 530, 1220, 904}};
        const auto g = sshd::FitLabel({600, 520}, {600, 520}, 200, 50, kView, gap, 240);
        assert(g && sshd::Within(BoxAt(*g, 200, 50), kView) && Clear(BoxAt(*g, 200, 50), gap));
    }
    // outward only (island names): the box never slides back over its anchor
    {
        const sshd::Box block[] = {{700, 400, 760, 600}};
        const auto r = sshd::FitLabel({600, 500}, {700, 500}, 160, 40, kView, block, 240, 100);
        assert(r && (*r)[0] - 80 >= 600.0f && Clear(BoxAt(*r, 160, 40), block));
    }
    // dev.38: the Map page view pans and zooms (runtime pan_zoom); the module fits each plate to the
    // view as drawn (@map_view_*), so for every view of a sweep (the whole area .. 2.5x the near
    // view, panned around the mark) a plate is either fully inside the viewport, clear of the
    // screen-fixed zones and by its mark, or hidden only where nothing near its mark fits. A mark
    // well inside the viewport and away from the zones is never hidden.
    {
        const double home_ppw = 1200.0 / 14400.0;
        const std::array<double, 2> mark{16280.0, 10728.0};
        const float w = 364, h = 54;
        const auto off = sshd::PlaceOffset(60, 300, w, h);
        int views = 0, shown = 0;
        for (const double zoom : {0.437, 0.6, 1.0, 1.7, 2.52}) {
            for (const double dx : {-9000.0, -6000.0, -1500.0, 0.0, 1500.0, 6000.0, 9000.0}) {
                for (const double dy : {-6000.0, 0.0, 4000.0}) {
                    sshd::MapView v;
                    v.ppw = home_ppw * zoom, v.x = 20, v.y = 20, v.w = 1200, v.h = 884;
                    v.cx = mark[0] + dx / zoom, v.cy = mark[1] + dy / zoom;
                    const auto a = v.ToScreen(mark[0], mark[1]);
                    const std::array<float, 2> native{a[0] + off[0], a[1] + off[1]};
                    const auto r = sshd::FitLabel(a, native, w, h, v.Rect(), zones, 120);
                    ++views;
                    const bool easy = a[0] > 20 + w + 140 && a[0] < 1220 - w - 140 && a[1] > 200 && a[1] < 760;
                    if (!r) {
                        assert(!easy);
                        continue;
                    }
                    ++shown;
                    const sshd::Box b = BoxAt(*r, w, h);
                    assert(sshd::Within(b, v.Rect()) && Clear(b, zones));
                    assert(Gap(a, b) <= 120.01f);
                }
            }
        }
        assert(views == 105 && shown >= 40 && shown < views); // some marks off screen: hidden
        std::printf("pan/zoom label sweep: %d of %d views show the plate, all inside and clear\n", shown, views);
    }
    // the map projection round trip (y up)
    {
        sshd::MapView v;
        v.cx = 1000, v.cy = 2000, v.ppw = 0.1, v.x = 20, v.y = 20, v.w = 1200, v.h = 884;
        const auto p = v.ToScreen(1500, 2500);
        assert(std::fabs(p[0] - 670.0f) < 0.01f && std::fabs(p[1] - 412.0f) < 0.01f);
        const auto w = v.ToWorld(p[0], p[1]);
        assert(std::fabs(w[0] - 1500) < 0.01 && std::fabs(w[1] - 2500) < 0.01);
    }
}

// The module end to end with the real art (SSHD_ROMFS = romfs roots): the Faron Woods plate in
// the capture 04 view and the Sky names in a Fly view, each published where its art lands fully
// inside the view and clear of the page's zones; hidden with no view.
size_t ReadRomfs(void* p, const char* path, uint64_t off, void* out, size_t size) {
    for (const auto& d : static_cast<State*>(p)->romfs) {
        std::ifstream f(d + "/" + path, std::ios::binary);
        if (!f)
            continue;
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        if (out == nullptr)
            return b.size();
        if (off >= b.size())
            return 0;
        const size_t n = std::min<size_t>(size, b.size() - off);
        std::memcpy(out, b.data() + off, n);
        return n;
    }
    return 0;
}

void LiveFit(State& s, EdenDsmodHostApi h, const char* roots) {
    std::stringstream ss(roots);
    for (std::string d; std::getline(ss, d, ':');)
        s.romfs.push_back(d);
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.struct_size = sizeof(h);
    h.title_id = 0x01002DA013484000ull;
    const uint8_t build[] = {0x54, 0x41, 0xAF, 0x3C, 0x89, 0xF9, 0x7E, 0x1D, 0x25, 0xA8,
                             0xCC, 0xF1, 0x4C, 0x74, 0x2B, 0xF8, 0x23, 0x36, 0xD8, 0xA8};
    std::memcpy(h.build_id, build, sizeof(build));
    h.read_romfs = ReadRomfs;
    h.get_f64 = [](void* p, const char* k, double fb) {
        const auto& v = static_cast<State*>(p)->views;
        const auto it = v.find(k);
        return it != v.end() ? it->second : fb;
    };
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    void* inst = api->create(&h, "");
    assert(inst);
    sshd::Art art([&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        const size_t n = ReadRomfs(&s, path.c_str(), 0, nullptr, 0);
        if (n == 0)
            return std::nullopt;
        std::vector<uint8_t> b(n);
        ReadRomfs(&s, path.c_str(), 0, b.data(), n);
        return b;
    });
    const auto set_view = [&](const std::string& area, double cx, double cy, double ppw, double w, double hh) {
        for (const auto& [f, val] : std::initializer_list<std::pair<const char*, double>>{
                 {"cx", cx}, {"cy", cy}, {"ppw", ppw}, {"x", 20}, {"y", 20}, {"w", w}, {"h", hh}, {"rot", 0}})
            s.views["@map_view_" + std::string(f) + ":" + area] = val;
    };
    // one Faron Woods mark (label 2, Impa met) facing 60 degrees on the hero's F402 map (u = -x, v = z)
    s.blocks.clear();
    s.words.clear();
    std::memset(s.story, 0, sizeof(s.story));
    s.story[0x0D] = 0x01;
    const auto put = [&](uint64_t a, size_t n) -> std::vector<uint8_t>& {
        s.blocks[a] = std::vector<uint8_t>(n, 0);
        return s.blocks[a];
    };
    {
        auto& root = put(0x1688510, 8);
        const uint64_t a = 0x10000;
        std::memcpy(root.data(), &a, 8);
        auto& n = put(0x10000, 40);
        const uint64_t v[5] = {0, 0, 0, 0, 0x23000};
        std::memcpy(n.data(), v, 40);
        auto& mm = put(0x23000, 0xA00);
        const uint32_t params = 0xFFFFFFFF;
        const uint16_t prof = 0x224;
        const int16_t yaw = static_cast<int16_t>(54250);
        const float pos[3] = {-16280.0f, 8706.0f, 10728.0f};
        std::memcpy(&mm[0xC], &params, 4);
        std::memcpy(&mm[0x10], &prof, 2);
        std::memcpy(&mm[0x13E], &yaw, 2);
        std::memcpy(&mm[0x144], pos, 12);
        const uint8_t f[0x18] = {0xFF, 0x07, 0x00, 0x00, 0xFF, 0xFF, 3, 3, 2, 0xFF, 0xFF, 0x00, 3, 0, 0, 0};
        std::memcpy(&mm[0x190], f, sizeof(f));
        put(kSlot + 0x5318, 2)[0] = 10;
        put(kSlot + 0x7BC, 4);
    }
    s.stage = "F402";
    s.scene_name = 0x6C616544;
    const auto ps = art.PlaceMetrics(2);
    assert(ps);
    const auto off = sshd::PlaceOffset(60, static_cast<float>(ps->tw), static_cast<float>(ps->bw),
                                       static_cast<float>(ps->bh));
    const sshd::Box map_zones[] = {kCompass, kReset, kTitle};
    // capture 04 (hero spawned at (-10500, 7700, 6800)): the live follow view (14400 units across,
    // centred on Link, read back from the host on the dev.28 run): the mark right of the compass's
    // left edge, the native plate past the viewport and under the compass
    const double ppw = 1200.0 / 14400.0;
    sshd::MapView v;
    v.ppw = ppw, v.x = 20, v.y = 20, v.w = 1200, v.h = 884;
    v.cx = 10529.3779, v.cy = 6819.0254;
    set_view("F401_m", v.cx, v.cy, ppw, 1200, 884);
    {
        const auto m = v.ToScreen(16280.0, 10728.0);
        const sshd::Box native = BoxAt({m[0] + off[0], m[1] + off[1]}, ps->bw, ps->bh);
        assert(!sshd::Within(native, v.Rect()) || !Clear(native, map_zones)); // dev.27 drew it here
    }
    api->sample(inst, &h);
    assert(s.ints["sshd.pl0.on"] == 1 && s.texts["sshd.pl0.src"] == "module:sshd:place/2/60/1");
    {
        const auto m = v.ToScreen(s.floats["sshd.pl0.u"], s.floats["sshd.pl0.v"]);
        const sshd::Box drawn = BoxAt({m[0] + off[0], m[1] + off[1]}, ps->bw, ps->bh);
        assert(sshd::Within(drawn, v.Rect()) && Clear(drawn, map_zones));
        assert(Gap(v.ToScreen(16280.0, 10728.0), drawn) <= 120.5f); // still by its mark
        std::printf("plate fit: box (%.0f, %.0f)-(%.0f, %.0f), %d x %d px\n", drawn.x0, drawn.y0, drawn.x1, drawn.y1,
                    ps->bw, ps->bh);
    }
    // dev.38: the same plate under pan / pinch views (the module refits it to each view the host
    // drew): wherever it is published it lands fully inside the viewport, clear of the zones and by
    // its mark; zoomed in on the mark it shows
    {
        int shown = 0, views = 0;
        for (const double zoom : {0.437, 1.0, 2.52}) {
            for (const double dx : {-3000.0, 0.0, 3000.0}) {
                sshd::MapView z = v;
                z.ppw = ppw * zoom, z.cx = 16280.0 + dx / zoom, z.cy = 10728.0;
                set_view("F401_m", z.cx, z.cy, z.ppw, 1200, 884);
                api->sample(inst, &h);
                ++views;
                if (s.ints["sshd.pl0.on"] != 1) {
                    assert(!(zoom == 2.52 && dx == 0.0));
                    continue;
                }
                ++shown;
                const auto m = z.ToScreen(s.floats["sshd.pl0.u"], s.floats["sshd.pl0.v"]);
                const sshd::Box drawn = BoxAt({m[0] + off[0], m[1] + off[1]}, ps->bw, ps->bh);
                assert(sshd::Within(drawn, z.Rect()) && Clear(drawn, map_zones));
                assert(Gap(z.ToScreen(16280.0, 10728.0), drawn) <= 120.5f);
            }
        }
        std::printf("plate fit under pan / zoom: %d of %d views shown, all inside and clear\n", shown, views);
        assert(shown >= 5);
    }
    // the mark far off screen: hidden; no Map page view (another page): hidden
    set_view("F401_m", v.cx - 40000, v.cy, ppw, 1200, 884);
    api->sample(inst, &h);
    assert(s.ints["sshd.pl0.on"] == 0);
    s.views.clear();
    api->sample(inst, &h);
    assert(s.ints["sshd.pl0.on"] == 0);

    // Fly page: the whole-Sky view centred on Skyloft, every discovered name clear of the zones
    const sshd::Box fly_zones[] = {{32, 26, 512, 146}, {1065, 21, 1215, 171}, {40, 180, 160, 620},
                                   {300, 790, 576, 882}, {40, 744, 286, 910},
                                   {930, 450, 1215, 902},  // dev.33: the flying page's D-pad cluster
                                   {1004, 190, 1220, 318}}; // dev.38: its Reset plank (dev.41 128 tall)
    s.blocks.clear();
    s.stage = "F020";
    std::memset(s.story, 0, sizeof(s.story));
    s.story[0x22] = 0x08 | 0x10 | 0x20 | 0x40 | 0x80; // 281..285
    s.story[0x23] = 0x01;                            // 286
    static constexpr double kIsl[7][2] = {{0.0, 0.0},           {-114296.0, 49995.0},  {102667.0, 47640.0},
                                          {130349.0, -108755.0}, {39922.0, -110282.0}, {-140495.0, -109061.0},
                                          {-170937.0, -50851.0}};
    // dev.38: also pinched in (2.5x, the bird near Skyloft): every name shown lies inside and clear
    for (const double zoom : {1.0, 2.5}) for (const double cy : {0.0, -30000.0, 30000.0}) {
        sshd::MapView fv;
        fv.cx = 0, fv.cy = cy, fv.ppw = 1200.0 / 380000.0 * zoom, fv.x = 20, fv.y = 20, fv.w = 1200, fv.h = 882;
        set_view("F020", fv.cx, fv.cy, fv.ppw, 1200, 882);
        api->sample(inst, &h);
        int shown = 0;
        std::string hidden;
        for (int i = 0; i < 7; ++i) {
            const std::string k = "sshd.isl" + std::to_string(i);
            const auto b = art.IsleNameBox(i, i == 0 ? 66 : 46);
            assert(b);
            if (s.ints[k + ".on"] != 1) {
                // hidden only where the native spot does not fit (and no outward move within reach)
                const auto isl = fv.ToScreen(kIsl[i][0], -kIsl[i][1]);
                const sshd::Box at{isl[0] + b->x0, isl[1] + b->y0, isl[0] + b->x1, isl[1] + b->y1};
                assert(!sshd::Within(at, fv.Rect()) || !Clear(at, fly_zones));
                hidden += " " + std::to_string(i);
                continue;
            }
            ++shown;
            const auto m = fv.ToScreen(s.floats[k + ".u"], s.floats[k + ".v"]);
            const sshd::Box drawn{m[0] + b->x0, m[1] + b->y0, m[0] + b->x1, m[1] + b->y1};
            assert(sshd::Within(drawn, fv.Rect()) && Clear(drawn, fly_zones));
            // a name only moves outwards (right) of its island, and stays by it
            const auto isl = fv.ToScreen(kIsl[i][0], -kIsl[i][1]);
            assert(drawn.x0 >= isl[0] + b->x0 - 0.5f);
            assert(Gap(isl, drawn) <= 90.5f);
        }
        std::printf("fly view zoom %.1f cy %.0f: %d of 7 names shown (hidden:%s)\n", zoom, cy, shown, hidden.c_str());
        assert(zoom > 1.0 || shown >= 5);
    }
    api->destroy(inst);
}
} // namespace

int main() {
    State s;
    EdenDsmodHostApi h{};
    h.userdata = &s;
    h.main_base = 0;
    h.read_memory = Read;
    h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<State*>(p)->ints[k] = v; };
    h.publish_f64 = [](void* p, const char* k, double v) { static_cast<State*>(p)->floats[k] = v; };
    h.publish_text = [](void* p, const char* k, const char* v) { static_cast<State*>(p)->texts[k] = v; };
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);

    // --- cone: native calibration on Sealed Grounds (F402 draws with F401's map, u = -x, v = z)
    s.stage = "F402";
    s.yaw = 14807; // native cone 81 degrees left of up
    api->sample(nullptr, &h);
    assert(s.ints["sshd.cone.on"] == 1);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/280");
    s.yaw = 1972; // native cone 11 degrees left of up
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/350");
    s.yaw = 0; // facing +z: up on a v = z map
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/0");
    s.stage = "F000"; // Skyloft (dev.32): +x right / +z down like the native map (flip_h, flip_v)
    s.yaw = 16384;    // facing +x: right
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/90");
    s.yaw = 0; // facing +z: down
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/180");
    s.yaw = 16384;
    s.yaw = 0;
    s.stage = "D100"; // flip_v: +z is down
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/180");
    s.stage = "F301"; // flip_h + flip_v: facing +x points right
    s.yaw = 16384;
    api->sample(nullptr, &h);
    assert(s.texts["sshd.cone.src"] == "module:sshd:cone/90");
    s.stage = "F020"; // the Sky: u = x, v = -z
    s.yaw = -16384;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.cone.on"] == 1 && s.texts["sshd.cone.src"] == "module:sshd:cone/270");
    s.stage = "F999"; // unknown stage, title, unreadable: no cone
    api->sample(nullptr, &h);
    assert(s.ints["sshd.cone.on"] == 0);
    s.stage = "F401";
    s.scene_name = 0x53656C74;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.cone.on"] == 0);
    s.scene_name = 0x6C616544;
    s.readable = false;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.cone.on"] == 0);
    s.readable = true;

    // --- Sky Goddess tags: chest flag 2 (story 229, Wii byte 0x1A bit 2 -> HD byte 0x1B bit 7)
    s.stage = "F020";
    s.yaw = 0;
    api->sample(nullptr, &h);
    for (int i = 0; i < 22; ++i)
        assert(s.ints["sshd.gc" + std::to_string(i) + ".on"] == 0); // no cube struck: no tag
    s.story[0x1B] = 0x80; // cube 229 struck
    api->sample(nullptr, &h);
    assert(s.ints["sshd.gc2.on"] == 1 && s.ints["sshd.gc.n"] == 1);
    assert(s.floats["sshd.gc2.u"] == -137465.0 && s.floats["sshd.gc2.v"] == -32051.0);
    s.words[kSlot + 0x2A64 + 4 * 21] = 1u << 2; // that chest opened (saved word): the tag goes
    api->sample(nullptr, &h);
    assert(s.ints["sshd.gc2.on"] == 0 && s.ints["sshd.gc.n"] == 0);
    s.words.clear();
    s.words[0x182E118] = 1u << 2; // opened just now (live word only)
    api->sample(nullptr, &h);
    assert(s.ints["sshd.gc2.on"] == 0);
    s.words.clear();
    s.stage = "F000"; // Skyloft: never tagged
    api->sample(nullptr, &h);
    assert(s.ints["sshd.gc.n"] == 0);

    // --- opened ordinary chests: hero's Sealed Grounds chest (F402 scene 10 flag 0)
    s.stage = "F402";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.ch0.on"] == 0); // closed: hidden (no Dungeon Map rule for closed chests)
    s.words[0x182E118] = 1;             // opened (live current-scene word)
    api->sample(nullptr, &h);
    assert(s.ints["sshd.ch0.on"] == 1 && s.texts["sshd.ch.src"] == "module:sshd:openchest");
    assert(s.floats["sshd.ch0.u"] == 1235.0 && s.floats["sshd.ch0.v"] == 12500.0);
    for (int i = 1; i < 5; ++i)
        assert(s.ints["sshd.ch" + std::to_string(i) + ".on"] == 0);
    s.words.clear();
    s.words[kSlot + 0x2A64 + 4 * 11] = 1u << 3; // Skyview chest flag 3 (saved)
    s.stage = "D100"; // dev.30: dungeon chests come from the live objects (sshd_dungeon_test)
    api->sample(nullptr, &h);
    assert(s.ints["sshd.ch1.on"] == 0 && s.ints["sshd.ch0.on"] == 0);
    s.stage = "F100"; // a stage the analysis did not decode: nothing
    api->sample(nullptr, &h);
    for (int i = 0; i < 5; ++i)
        assert(s.ints["sshd.ch" + std::to_string(i) + ".on"] == 0);

    // --- live objects (dev.27): hero Sealed Grounds, F402 scene 10. The tree: root -> A; A's
    // children B (east statue), C (door of a double door), D (its partner, not drawn), E (the
    // Faron Woods map mark).
    s.words.clear();
    const auto put = [&](uint64_t a, size_t n) -> std::vector<uint8_t>& {
        s.blocks[a] = std::vector<uint8_t>(n, 0);
        return s.blocks[a];
    };
    const auto node = [&](uint64_t a, uint64_t parent, uint64_t child, uint64_t sib, uint64_t obj) {
        auto& b = put(a, 40);
        const uint64_t v[5] = {parent, child, 0, sib, obj};
        std::memcpy(b.data(), v, 40);
    };
    const auto obj = [&](uint64_t a, uint16_t prof, uint32_t params, float x, float y, float z, int16_t yaw,
                         int8_t room) -> std::vector<uint8_t>& {
        auto& b = put(a, 0xA00);
        std::memcpy(&b[0xC], &params, 4);
        std::memcpy(&b[0x10], &prof, 2);
        b[0x16] = 2;
        std::memcpy(&b[0x13E], &yaw, 2);
        const float p[3] = {x, y, z};
        std::memcpy(&b[0x144], p, 12);
        b[0x17C] = static_cast<uint8_t>(room);
        return b;
    };
    {
        auto& root = put(0x1688510, 8);
        const uint64_t a = 0x10000;
        std::memcpy(root.data(), &a, 8);
    }
    node(0x10000, 0, 0x10100, 0, 0);
    node(0x10100, 0x10000, 0, 0x10200, 0x20000);
    node(0x10200, 0x10000, 0, 0x10300, 0x21000);
    node(0x10300, 0x10000, 0, 0x10400, 0x22000);
    node(0x10400, 0x10000, 0, 0, 0x23000);
    {
        auto& st = obj(0x20000, 0x216, 0x0105011F, -11029.0f, 7500.0f, 6279.0f, 14199, 1);
        const uint64_t pmf = 0x8BA1BC; // main base 0 in this test
        std::memcpy(&st[0x928], &pmf, 8);
        auto& d = obj(0x21000, 0xCD, 0x104, -1760.0f, 7600.0f, 10499.0f, -16384, 1);
        d[0x7C0] = 1;
        const uint64_t partner = 0x22000;
        std::memcpy(&d[0x748], &partner, 8);
        obj(0x22000, 0xCD, 0x104, -1760.0f, 7600.0f, 10341.0f, 16384, 1); // +0x7C0 = 0: not drawn
        auto& mm = obj(0x23000, 0x224, 0xFFFFFFFF, -16280.0f, 8706.0f, 10728.0f, static_cast<int16_t>(54250), 0);
        mm[0x16] = 3;
        const uint8_t f[0x18] = {0xFF, 0x07, 0x00, 0x00, 0xFF, 0xFF, 3, 3, 2, 0xFF, 0xFF, 0x00, 3, 0, 0, 0};
        std::memcpy(&mm[0x190], f, sizeof(f));
    }
    {
        auto& scene = put(kSlot + 0x5318, 2);
        scene[0] = 10;
    }
    auto& flags = put(kSlot + 0x1A64 + 160, 16); // scene 10's flags
    auto& discovered = put(kSlot + 0x7BC, 4);
    // dev.33 door gate (Skyloft analysis): the stage's STIF record (map
    // context +0x11, region +0xF: the Sealed Grounds are context 0, region 1) and the scene's dungeon
    // flags through the native mapping (record 2 = word 0 bit 1, record 8 = word 7, rooms 0..15)
    const auto ptr = [&](uint64_t a, uint64_t v) { std::memcpy(put(a, 8).data(), &v, 8); };
    ptr(0x2BF9DA8, 0x7100000);
    ptr(0x7100000 + 0xE7D8, 0x7110000);
    auto& stif = put(0x7110000, 0x20);
    stif[0x11] = 0;
    stif[0xF] = 1;
    ptr(0x182E120, 0x7120000);
    {
        auto& dm = put(0x7120000, 8);
        dm[2] = 10; // scene 10
    }
    ptr(0x7120008, 0x7121000);
    ptr(0x7121000 + 8, 0x7122000);
    {
        auto& map = put(0x7122000, 64);
        map[4] = 0, map[5] = 0x10;  // record 2: word 0, shift 1, one bit
        map[16] = 7, map[17] = 0x0F; // record 8: word 7, all 16 bits
    }
    auto& drow = put(kSlot + 0xA64 + 160, 16);
    drow[14] = 0x02; // room 1 visited
    s.stage = "F402";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.st0.on"] == 0); // statue flag 31 clear: not activated, hidden
    assert(s.ints["sshd.dr0.on"] == 1 && s.ints["sshd.dr1.on"] == 0);
    drow[14] = 0; // the door's room not visited, no dungeon flag 2: no bar
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 0);
    drow[0] = 0x02; // dungeon flag 2: every candidate door
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 1);
    stif[0xF] = 0, stif[0x11] = 2; // Skyloft's context: never a bar
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 0);
    stif[0x11] = 0; // an ordinary field: never a bar
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 0);
    stif[0x11] = 3; // an interior: always (dev.34: confirmed live in F001r, the shown floor's bars)
    drow[0] = 0;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 1);
    stif[0x11] = 0, stif[0xF] = 1;
    drow[14] = 0x02;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 1 && s.ints["sshd.dr1.on"] == 0);
    // the bar 70 units in from the drawn door, across its facing (+x -> left on u = -x: vertical)
    assert(s.floats["sshd.dr0.u"] == 1760.0 && std::fabs(s.floats["sshd.dr0.v"] - 10429.0) < 0.01);
    assert(s.texts["sshd.dr0.src"] == "module:sshd:door/90");
    // Faron Woods: label 2 needs story 130 (Impa met), else the native "?" plate
    // (dev.28: shown only once placed in the Map page's view; with no instance (no plate size) and
    // no view the plate fails closed, its mark published as is)
    assert(s.ints["sshd.pl0.on"] == 0 && s.texts["sshd.pl0.src"] == "module:sshd:place/12/60/1");
    assert(s.floats["sshd.pl0.u"] == 16280.0 && s.floats["sshd.pl0.v"] == 10728.0);
    flags[3] = 0x80; // scene flag 31 (word 1 bit 15)
    s.story[0x0D] = 0x01; // story 130
    api->sample(nullptr, &h);
    assert(s.ints["sshd.st0.on"] == 1 && s.texts["sshd.st0.src"] == "module:sshd:statue/1/0");
    assert(s.floats["sshd.st0.u"] == 11029.0 && s.floats["sshd.st0.v"] == 6279.0);
    // dev.41: a parent chain that loops (the root its own parent: a transient state
    // of the live tree) ends every walk of it after the climb cap instead of hanging the sample
    // (the live objects, the story getter's probe and the D-pad / Gear sword's active-file probe,
    // which hung here before); the objects read before it stay
    std::memcpy(s.blocks[0x10000].data(), "\x00\x00\x01\x00\x00\x00\x00\x00", 8); // parent = 0x10000
    api->sample(nullptr, &h);
    assert(s.ints["sshd.st0.on"] == 1 && s.ints["sshd.dr0.on"] == 1);
    std::memset(s.blocks[0x10000].data(), 0, 8);
    assert(s.texts["sshd.pl0.src"] == "module:sshd:place/2/60/1");
    s.story[0x0D] = 0;
    discovered[0] = 0x08; // Lanayru Mine (label 7)'s bit only: a label 7 mark would show
    s.blocks[0x23000][0x198] = 7;
    api->sample(nullptr, &h);
    assert(s.texts["sshd.pl0.src"] == "module:sshd:place/12/60/1");
    discovered[0] = 0x80;
    api->sample(nullptr, &h);
    assert(s.texts["sshd.pl0.src"] == "module:sshd:place/7/60/1");
    s.blocks[0x23000][0x196] = 0; // scene SET with flag 255: false for SET and UNSET (the dead duplicate)
    s.texts.erase("sshd.pl0.src");
    api->sample(nullptr, &h);
    assert(s.ints["sshd.pl0.on"] == 0 && !s.texts.count("sshd.pl0.src"));
    s.blocks[0x23000][0x196] = 3;
    // dev.31: a story-conditional plate (Faron's Deep Woods mark: story 58, word 2 >> 13 = story
    // byte 5 bit 5 through the native getter): listed only while its story flag is set
    s.blocks[0x23000][0x197] = 0;
    const uint16_t s58 = 58;
    std::memcpy(&s.blocks[0x23000][0x190], &s58, 2);
    s.texts.erase("sshd.pl0.src");
    api->sample(nullptr, &h);
    assert(!s.texts.count("sshd.pl0.src"));
    s.story[5] = 0x20;
    api->sample(nullptr, &h);
    assert(s.texts["sshd.pl0.src"] == "module:sshd:place/7/60/1");
    s.story[5] = 0;
    s.blocks[0x23000][0x197] = 3;
    // dev.31 NPC balloons, landmark glyphs and Beedle: three more objects on the tree
    s.blocks[0x10400][24] = 0x00, s.blocks[0x10400][25] = 0x05, s.blocks[0x10400][26] = 0x01; // sibling 0x10500
    node(0x10500, 0x10000, 0, 0x10600, 0x24000);
    node(0x10600, 0x10000, 0, 0x10700, 0x25000);
    node(0x10700, 0x10000, 0, 0, 0x26000);
    const auto mark = [&](uint64_t a, uint8_t icon, uint16_t story, float x, float z) {
        auto& m = obj(a, 0x224, 0xFFFFFFFF, x, 100.0f, z, 0, 0);
        m[0x16] = 3;
        const uint8_t f[0x18] = {static_cast<uint8_t>(story & 0xFF), static_cast<uint8_t>(story >> 8), 62, 0, 0xFF, 0xFF, 3, 0,
                                 0xFF, icon, 0xFF, 0x00, 4, 0, 0, 0};
        std::memcpy(&m[0x190], f, sizeof(f));
    };
    mark(0x24000, 18, 180, -3940.0f, -15400.0f); // the Thrill Digger Mogma (field), story 180
    mark(0x25000, 24, 183, 5000.0f, -9000.0f);   // a landmark glyph, story 183
    obj(0x26000, 0x69, 0, 300.0f, 2400.0f, -3500.0f, 0, 0); // Beedle's airshop
    s.stage = "F200";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 0 && s.ints["sshd.cv0.on"] == 0 && s.ints["sshd.bs0.on"] == 0);
    s.story[15] = 0x08 | 0x40; // story 180, 183
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 1 && s.texts["sshd.nb0.src"] == "module:sshd:balloon/18");
    assert(s.floats["sshd.nb0.u"] == -3940.0 && s.floats["sshd.nb0.v"] == 15400.0); // Eldin (dev.32): u = x, v = -z
    assert(s.ints["sshd.cv0.on"] == 1 && s.texts["sshd.cv.src"] == "module:sshd:cave");
    assert(s.ints["sshd.bs0.on"] == 0); // Beedle: the Sky layout only
    // a field portrait on the Sky map: not drawn; Fledge (23, story 400) by day only
    s.stage = "F020";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 0 && s.ints["sshd.cv0.on"] == 0);
    s.blocks[0x24000][0x199] = 23;
    const uint16_t s400 = 400;
    std::memcpy(&s.blocks[0x24000][0x190], &s400, 2);
    s.story[57] = 0x01; // story 400
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 1 && s.texts["sshd.nb0.src"] == "module:sshd:balloon/23");
    assert(s.floats["sshd.nb0.u"] == -3940.0 && s.floats["sshd.nb0.v"] == 15400.0); // Sky: u = x, v = -z
    // Beedle by day once his shop was entered (story 709, word 37 >> 15)
    assert(s.ints["sshd.bs0.on"] == 0);
    s.story[75] = 0x80;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.bs0.on"] == 1 && s.texts["sshd.bs0.src"] == "module:sshd:beedle/1");
    auto& tod = put(0x2BF98FD, 1);
    tod[0] = 1; // night: Fledge and Beedle go
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 0 && s.ints["sshd.bs0.on"] == 0);
    tod[0] = 0;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.nb0.on"] == 1);
    // dev.33 red common targets (Sky class only, Skyloft analysis C2a): the
    // landmark mark turned into icon 255 / the Bazaar's 3, condition story 183 (set)
    s.blocks[0x25000][0x199] = 255;
    s.stage = "F000";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.tg0.on"] == 1 && s.texts["sshd.tg0.src"] == "module:sshd:target/0");
    assert(s.floats["sshd.tg0.u"] == 5000.0 && s.floats["sshd.tg0.v"] == 9000.0); // Skyloft: u = x, v = -z
    s.blocks[0x25000][0x199] = 3;
    api->sample(nullptr, &h);
    assert(s.ints["sshd.tg0.on"] == 1 && s.texts["sshd.tg0.src"] == "module:sshd:target/1");
    s.blocks[0x25000][0x199] = 1; // the hidden goddess pane: nothing
    api->sample(nullptr, &h);
    assert(s.ints["sshd.tg0.on"] == 0);
    s.blocks[0x25000][0x199] = 255;
    s.story[15] = 0x08; // story 183 clear: the mark's condition fails
    api->sample(nullptr, &h);
    assert(s.ints["sshd.tg0.on"] == 0);
    s.story[15] = 0x08 | 0x40;
    s.stage = "F200"; // the field class rejects them
    api->sample(nullptr, &h);
    assert(s.ints["sshd.tg0.on"] == 0);
    s.blocks[0x25000][0x199] = 24;
    // dev.33 Bug Medal bug locations (Skyloft analysis C2c): Beedle's object turned into an insect tag
    {
        auto& tag = s.blocks[0x26000];
        const uint16_t prof = 0x1C1;
        std::memcpy(&tag[0x10], &prof, 2);
        const int32_t count = 1;
        std::memcpy(&tag[0x3D0], &count, 4);
        const uint64_t insect = 0x7777000;
        std::memcpy(&tag[0x210], &insect, 8);
        auto& slots = put(kSlot + 0x9E4 + 2 * 5, 2); // Adventure Pouch: 4 slots
        slots[0] = 0x40;
        auto& pouch = put(kSlot + 0x7C0, 16);
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 0); // no medal
        pouch[8] = 0x46; // the Bug Medal in slot 2
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 1 && s.texts["sshd.bg.src"] == "module:sshd:bug");
        assert(s.floats["sshd.bg0.u"] == 300.0 && s.floats["sshd.bg0.v"] == 3500.0);
        tag[0x3D6] = 1; // the tag's own off byte
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 0);
        tag[0x3D6] = 0;
        std::memset(&tag[0x210], 0, 8); // no live insect
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 0);
        tag[0x3D7] = 1, tag[0x3D8] = 1; // the alternative live-insect bytes
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 1);
        s.stage = "F000"; // the Sky class too
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 1);
        // dev.34 (item 10): profile 0x1A7 "Bee" dots on the field class behind the same medal gate
        // (main+0xC7DFD8), none on the Sky class (it emits another kind there)
        {
            const uint16_t bee = 0x1A7;
            std::memcpy(&tag[0x10], &bee, 2);
            tag[0x3D7] = 0, tag[0x3D8] = 0; // the insect-tag tests do not apply to a bee
            api->sample(nullptr, &h);
            assert(s.ints["sshd.bg0.on"] == 0); // Skyloft: the Sky class
            s.stage = "F200";
            api->sample(nullptr, &h);
            assert(s.ints["sshd.bg0.on"] == 1);
            pouch[8] = 0; // no medal: no bee dot
            api->sample(nullptr, &h);
            assert(s.ints["sshd.bg0.on"] == 0);
            pouch[8] = 0x46;
            const uint16_t prof1c1 = 0x1C1;
            std::memcpy(&tag[0x10], &prof1c1, 2);
            tag[0x3D7] = 1, tag[0x3D8] = 1;
            s.stage = "F000";
        }
        auto& special = put(0x192FB58, 8);
        special[0] = 1;
        s.words[0x166CC10] = 9; // the special mode: no bugs
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bg0.on"] == 0);
        s.blocks.erase(0x192FB58);
        s.words.erase(0x166CC10);
        pouch[8] = 0;
        s.blocks.erase(kSlot + 0x9E4 + 2 * 5);
        s.blocks.erase(kSlot + 0x7C0);
        const uint16_t beedle = 0x69;
        std::memcpy(&tag[0x10], &beedle, 2);
        s.stage = "F020";
        api->sample(nullptr, &h);
    }
    // the special-scene story buffer: every story term fails closed
    {
        auto& scn = obj(0x27000, 0, 0, 0, 0, 0, 0, 0);
        scn[0x16] = 1; // the first group-1 object, profile 0; the save manager's +0xCD06 reads 0
        node(0x10000, 0, 0x10050, 0, 0);
        node(0x10050, 0x10000, 0, 0x10100, 0x27000);
        api->sample(nullptr, &h);
        assert(s.ints["sshd.nb0.on"] == 0 && s.ints["sshd.story.why"] == 2);
        node(0x10000, 0, 0x10100, 0, 0);
        api->sample(nullptr, &h);
        assert(s.ints["sshd.nb0.on"] == 1 && s.ints["sshd.story.why"] == 0);
    }
    for (uint64_t a : {0x10500, 0x10600, 0x10700, 0x24000, 0x25000, 0x26000, 0x27000, 0x2BF98FD})
        s.blocks.erase(a);
    std::memset(s.story, 0, sizeof(s.story));
    s.blocks[0x10400][24] = 0, s.blocks[0x10400][25] = 0, s.blocks[0x10400][26] = 0;
    s.stage = "F402";
    {   // an unknown activation function: the statue fails closed
        const uint64_t other = 0x123456;
        std::memcpy(&s.blocks[0x20000][0x928], &other, 8);
        api->sample(nullptr, &h);
        assert(s.ints["sshd.st0.on"] == 0);
        const uint64_t always = 0x8BA1EC;
        std::memcpy(&s.blocks[0x20000][0x928], &always, 8);
        flags[3] = 0;
        api->sample(nullptr, &h);
        assert(s.ints["sshd.st0.on"] == 1);
    }
    s.blocks[0x20000][0x15D] = 0x01; // hidden object (property 0x100)
    api->sample(nullptr, &h);
    assert(s.ints["sshd.st0.on"] == 0);
    s.blocks[0x20000][0x15D] = 0;
    s.stage = "D100"; // dungeon: no door bars, no plates; the statue (always-activated here) stays
    api->sample(nullptr, &h);
    assert(s.ints["sshd.dr0.on"] == 0 && s.ints["sshd.pl0.on"] == 0 && s.ints["sshd.st0.on"] == 1);
    assert(s.texts["sshd.st0.src"] == "module:sshd:statue/1/0");
    s.stage = "F401";
    s.scene_name = 0x53656C74; // title: nothing
    api->sample(nullptr, &h);
    assert(s.ints["sshd.st0.on"] == 0 && s.ints["sshd.dr0.on"] == 0 && s.ints["sshd.pl0.on"] == 0);
    s.scene_name = 0x6C616544;

    // --- Sky layers: thunderhead cover by story 379, columns by 46 / 47 / 48, names by 281..286
    s.blocks.clear();
    std::memset(s.story, 0, sizeof(s.story));
    s.stage = "F020";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.th.on"] == 1 && s.texts["sshd.th.src"] == "module:sshd:thunderhead/0");
    assert(s.floats["sshd.th.u"] == -135210.0 && s.floats["sshd.th.v"] == 84790.0);
    for (int i = 0; i < 3; ++i)
        assert(s.ints["sshd.col" + std::to_string(i) + ".on"] == 0);
    assert(s.ints["sshd.isl.known"] == 1); // Skyloft
    for (int i = 0; i < 7; ++i) // no Fly view / name size here: every name fails closed
        assert(s.ints["sshd.isl" + std::to_string(i) + ".on"] == 0);
    s.story[0x2C] = 0x02;          // 379 thunderhead entered
    s.story[0x0E] = 0x80;          // 46 Faron pillar
    s.story[0x0F] = 0x02;          // 48 Lanayru pillar
    s.story[0x22] = 0x10 | 0x40;   // 282 Pumpkin, 284 Bamboo
    s.story[0x23] = 0x01;          // 286 Bug Rock
    api->sample(nullptr, &h);
    assert(s.texts["sshd.th.src"] == "module:sshd:thunderhead/1");
    assert(s.ints["sshd.col0.on"] == 1 && s.ints["sshd.col1.on"] == 0 && s.ints["sshd.col2.on"] == 1);
    assert(s.texts["sshd.col2.src"] == "module:sshd:column/2" && s.floats["sshd.col2.v"] == -63800.0);
    assert(s.ints["sshd.isl.known"] == (1 | 4 | 16 | 64)); // Skyloft, Pumpkin, Bamboo, Bug Rock
    assert(s.texts["sshd.isl2.src"] == "module:sshd:islename/2/46" && s.floats["sshd.isl2.u"] == 102667.0);
    // dev.35 (item 5): Inside the Thunderhead (F023) is drawn on the Sky picture in the Sky's frame:
    // the ring (entered), the columns and Link's cone; no generic frame; no island names (no Fly view)
    s.stage = "F023";
    api->sample(nullptr, &h);
    assert(s.ints["sshd.th.on"] == 1 && s.texts["sshd.th.src"] == "module:sshd:thunderhead/1");
    assert(s.ints["sshd.col0.on"] == 1 && s.ints["sshd.cone.on"] == 1 && s.ints["sshd.gen.on"] == 0);
    assert(s.ints["sshd.isl2.on"] == 0 && sshd::FindArea("F023") == nullptr && sshd::FindGen("F023") == nullptr);
    s.stage = "F020";
    s.stage = "F401"; // off the Sky: no layers
    api->sample(nullptr, &h);
    assert(s.ints["sshd.th.on"] == 0 && s.ints["sshd.col0.on"] == 0 && s.ints["sshd.isl0.on"] == 0 &&
           s.ints["sshd.isl.known"] == 0);
    assert(s.ints["sshd.gen.on"] == 0); // a dedicated area: no generic frame

    // dev.34 (item 2): a stage without a dedicated area draws its own faces in the GEN frame
    {
        const float pos[3] = {-6481.31f, 30.0f, -5833.98f};
        std::memcpy(put(0x623E7C4, 12).data(), pos, 12);
        s.stage = "F102_1";
        api->sample(nullptr, &h);
        const auto* g = sshd::FindGen("F102_1");
        assert(g != nullptr && g->rot == 0 && g->name_id == 4);
        const auto f = sshd::GenFrameOf(*g);
        assert(s.ints["sshd.gen.on"] == 1 && s.ints["sshd.gen.titled"] == 1);
        assert(std::fabs(s.floats["sshd.gen.lu"] - f.s * (pos[0] - f.cx)) < 1e-3);
        assert(std::fabs(s.floats["sshd.gen.lv"] + f.s * (pos[2] - f.cz)) < 1e-3);
        assert(std::fabs(s.floats["sshd.gen.lu"]) < sshd::kGenHalf && std::fabs(s.floats["sshd.gen.lv"]) < sshd::kGenHalf);
        assert(s.texts["sshd.gen.src"] == "module:sshd:map/F102_1");
        assert(s.texts["sshd.gen.title"] == "module:sshd:ctitle/title/34/760/67/MAP_04_S");
        assert(s.texts["sshd.gen.ptext"] == "module:sshd:ctitle/plate/34/520/86/MAP_04");
        assert(s.texts["sshd.gen.plate"] == "module:sshd:areaplate/680/120/1");
        // the art's generic area agrees: Link's frame position is his picture position
        const auto* a = sshd::FindArea("F102_1");
        assert(a != nullptr && a->gen && a->flip_h && a->flip_v);
        const double px = (pos[0] - a->xmin) * a->ppu + sshd::kMapPad;
        const double fx = (s.floats["sshd.gen.lu"] + sshd::kGenHalf) * 0.0625 + sshd::kMapPad;
        assert(std::fabs(px - fx) < 1.0);
        // a quarter-turned stage (Volcano Summit, MapArea yaw 0x4000): map x = -z, map z = x
        s.stage = "F201_1";
        api->sample(nullptr, &h);
        const auto* g2 = sshd::FindGen("F201_1");
        const auto f2 = sshd::GenFrameOf(*g2);
        assert(g2->rot == 1 && std::fabs(s.floats["sshd.gen.lu"] - f2.s * (-pos[2] - f2.cx)) < 1e-3);
        assert(std::fabs(s.floats["sshd.gen.lv"] + f2.s * (pos[0] - f2.cz)) < 1e-3);
        assert(s.texts["sshd.gen.ptext"] == "module:sshd:ctitle/plate/34/520/86/MAP_06");
        assert(s.texts["sshd.gen.plate"] == "module:sshd:areaplate/680/120/2");
        // a cutscene stage: paper and Link, no title
        s.stage = "Demo";
        api->sample(nullptr, &h);
        assert(s.ints["sshd.gen.on"] == 1 && s.ints["sshd.gen.titled"] == 0);
        // the title screen: nothing
        s.scene_name = 0x53656C74;
        api->sample(nullptr, &h);
        assert(s.ints["sshd.gen.on"] == 0);
        s.scene_name = 0x6C616544;
        s.blocks.erase(0x623E7C4);
        s.stage = "F401";
    }

    LabelFit();
    if (const char* roots = std::getenv("SSHD_ROMFS"))
        LiveFit(s, h, roots);
    else
        std::printf("sshd markers: ok (set SSHD_ROMFS for the module's plate / name fit with real art)\n");
    return 0;
}
