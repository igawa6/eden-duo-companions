// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// FINAL FANTASY RESONANCE (0100606026F12000) companion module, update 1.0.1.
// Party, equipment, items, the live map (towns, dungeons and the world map, with pinch zoom) and the battle, drawn in the
// game's own look from the player's game files (fonts, icons, portraits, map captures, English text). Nothing from the game
// is shipped. It never writes the game's memory: in a battle, a tap on the command window presses the same buttons the
// player would (the package's enforce rules), step by step, checking the game's own cursor after every press.
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "ffr_assets.h"
#include "ffr_draw.h"
#include "ffr_helper_mailbox.h"
#include "ffr_pages.h"
#include "ffr_reader.h"


namespace {

constexpr uint64_t kTitle = 0x0100606026F12000ull;
const char* kPrefix = "module:ffr:page:";
const char* kKeys = "module:ffr:";  // every picture the package asks the module for

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The map widgets' coordinates (the manifest's areas): a town or dungeon in a 4096 square, the world in a 16384 square, y up.
constexpr double kTownN = 4096, kWorldN = 4096;
// Islands draw as markers of a few world sizes (the manifest's island groups, one per size, a square root of two apart); each
// picture is scaled into the smallest class that holds it, so none loses much resolution.
constexpr double kIslandSizes[] = {1024, 724, 512, 362, 256, 181};
// The town picture's margin left and right of the capture's square (map units; the manifest's TOWN_PAD): the menu background
// there, so a view as wide as the widget stays on the picture.
constexpr int kTownPad = 904;
constexpr int kIslandClasses = 6, kIslandsPerClass = 12, kMaxIcons = 64, kMaxGoals = 16;  // (the manifest's MAX_GOALS)
constexpr int kMaxMarks = 64;  // the world map's places (the manifest's MAX_MARKS)
// The map's own zoom (the +/- buttons; the map always follows the player at it; 1 = a town's whole picture, the world's usual window), kept
// per kind of map for the next visit
constexpr double kZoomStep = 1.25, kZoomMin[2] = {0.5, 0.2}, kZoomMax[2] = {40.0, 40.0};
const char* kMapWidget[2] = {"ffr_town_map", "ffr_world_map"};
const char* kMapArea[2] = {"ffr_town", "ffr_world"};
struct MapFrame {
    int mode = 0;  // 0 none, 1 an area's capture, 2 the world
    double ox = 0, oy = 0, k = 1, n = kTownN;  // map = (land - origin) * k, y flipped
    std::string base;                         // mode 1: the capture picture's key
    double v[4] = {};                         // the default view: x0 y0 x1 y1
    struct Pin { double x, y; std::string src; int cls = 0; bool main = true; };
    std::vector<Pin> islands, icons, goal;
    std::vector<Pin> marks;     // the world map's places (the reader's landmarks), their picture chosen when published
    std::vector<int> mark_ids;  // each one's map id
    double X(double lx) const { return (lx - ox) * k; }
    double Y(double ly) const { return n - (ly - oy) * k; }
};

// The world map's coordinates from its islands: every island's capture covers a square of side max(bw, bh) around its location.
// They draw as markers of a few world sizes (kIslandSizes), each picture scaled inside its own square (the module pads it: "isl:"
// keys). False when there is none.
bool WorldInto(MapFrame& f, const std::vector<ffr::Island>& isl) {
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18, smax = 0;
    for (auto& is : isl) {
        const double sd = std::max(is.bw, is.bh);
        x0 = std::min(x0, is.wx - sd / 2), x1 = std::max(x1, is.wx + sd / 2);
        y0 = std::min(y0, is.wy - sd / 2), y1 = std::max(y1, is.wy + sd / 2);
        smax = std::max(smax, sd);
    }
    if (smax <= 0) return false;
    const double span = std::max(x1 - x0, y1 - y0);
    f.mode = 2, f.n = kWorldN;
    f.k = std::min(kWorldN * 0.92 / span, kIslandSizes[0] / smax);
    f.ox = (x0 + x1) / 2 - kWorldN / 2 / f.k, f.oy = (y0 + y1) / 2 - kWorldN / 2 / f.k;
    int per[kIslandClasses] = {};
    for (auto& is : isl) {
        const double sz = std::max(is.bw, is.bh) * f.k;
        int c = 0;
        while (c + 1 < kIslandClasses && kIslandSizes[c + 1] >= sz) ++c;
        if (per[c] >= kIslandsPerClass) continue;
        ++per[c];
        // The picture: 1.5 times its class's world size in pixels, at most 1024 (the capture's own size). The host holds 64 MiB of
        // module pictures and drops the oldest loaded first, in use or not: the whole world (ocean, islands) must stay well
        // under that, or its pictures reload over and over (the flicker).
        char frac[64];
        std::snprintf(frac, sizeof frac, "%d:%.3f", int(std::min(1024.0, 1.5 * kIslandSizes[c])), std::min(1.0, sz / kIslandSizes[c]));
        f.islands.push_back({f.X(is.wx), f.Y(is.wy), std::string(kKeys) + "isl:" + frac + ":" + is.texture, c});
    }
    return true;
}

void MarksInto(MapFrame& f, const ffr::MapState& m) {  // the world map's places
    for (auto& l : m.landmarks) {
        const double x = f.X(l.x), y = f.Y(l.y);
        if (x < 0 || y < 0 || x > f.n || y > f.n || int(f.marks.size()) >= kMaxMarks) continue;
        f.marks.push_back({x, y, ""});
        f.mark_ids.push_back(l.map_id);
    }
}

MapFrame Frame(const ffr::MapState& m) {
    MapFrame f;
    if (!m.valid) return f;
    if (m.world) {
        if (!WorldInto(f, m.islands)) return f;
        MarksInto(f, m);
        // The default view follows the player (Sample), about as close as the game's own map opens
    } else if (!m.texture.empty() && m.bw > 0 && m.bh > 0) {
        const double side = std::max(m.bw, m.bh);
        f.mode = 1, f.n = kTownN, f.k = kTownN / side;
        f.ox = m.wx - side / 2, f.oy = m.wy - side / 2;
        f.base = std::string(kKeys) + "cap:" + m.texture;
        f.v[0] = f.X(m.wx - m.bw / 2), f.v[2] = f.X(m.wx + m.bw / 2), f.v[1] = f.Y(m.wy + m.bh / 2), f.v[3] = f.Y(m.wy - m.bh / 2);
    } else {
        return f;
    }
    for (auto& g : m.goals) {  // the objectives in this map
        const double x = f.X(g.x), y = f.Y(g.y);
        if (x < 0 || y < 0 || x > f.n || y > f.n || int(f.goal.size()) >= kMaxGoals) continue;
        f.goal.push_back({x, y, std::string(kKeys) + (g.main && g.side ? "goal:both" : g.main ? "goal" : "goal:side"), g.side ? 1 : 0, g.main});
    }
    for (auto& ic : m.icons) {  // (the reader leaves the world map's empty)
        const double x = f.X(ic.x), y = f.Y(ic.y);
        if (x < 0 || y < 0 || x > f.n || y > f.n || int(f.icons.size()) >= kMaxIcons) continue;
        f.icons.push_back({x, y, std::string(kKeys) + "ui:Icon/MAP/" + ic.icon});
    }
    return f;
}

// The world map seen from a town or dungeon (the scope switch): its islands and places, read in any map. On the world map, the
// frame itself.
MapFrame WorldFrame(const ffr::MapState& m) {
    if (m.world) return Frame(m);
    MapFrame f;
    if (!m.valid || !WorldInto(f, m.world_islands)) return MapFrame{};
    MarksInto(f, m);
    return f;
}

// Taps on the command window become the presses a player would make. One press at a time; the next only once the game's
// cursor has had time to move, and A only when the cursor (or the target) is the one tapped.
enum Btn { kBtnNone = -1, kBtnA, kBtnB, kBtnUp, kBtnDown, kBtnLeft, kBtnRight, kBtnMinus, kBtnZL, kBtnR, kBtnCount };
const char* kBtnPoints[kBtnCount] = {"ffr.drv.a", "ffr.drv.b", "ffr.drv.up", "ffr.drv.down", "ffr.drv.left", "ffr.drv.right",
                                     "ffr.drv.minus", "ffr.drv.zl", "ffr.drv.r"};
struct Drive {
    int kind = 0;       // 0 idle, 1 choose an entry, 2 choose a target, 3 back, 4 a resonance (want: its EUINavigation direction)
    int want = -1;      // the entry index
    uint64_t unit = 0;  // kind 2: the CPP_BtlUnit to target
    ffr::ActionRef key;  // the entry tapped (it must still be there)
    uint64_t start = 0, next = 0;
    int presses = 0, dir = 0, dir_presses = 0;
};

// A fast travel, through the game's own world map as a player would make it: Minus opens the map, ZL its destination list, R
// moves to the list's category that holds the place, the d-pad to it, A asks the game to travel. The game's own dialog then
// asks (or says why not) on the game screen; once it closes without travelling, B closes the list and the map. Each step waits
// for the game to show the last one (the reader's WorldMenu).
// With the package's guest helper installed (kDirect) none of that: the helper runs the game's own travel on the game thread,
// the code the dialog's Yes runs, after the game's own rules for it (native/helper/ffr_guest_helper.S).
struct Travel {
    enum Phase { kIdle, kDirect, kOpenMap, kOpenList, kWaitList, kCategory, kEntry, kWaitDialog, kDialog, kCloseList, kCloseMap };
    int phase = kIdle;
    int landing = -1;                // the place's landing point (the helper's travel), -1 not known
    bool sent = false, answered = false;  // kDirect: the request is in the mailbox; the helper answered it
    int target = -1, from_map = -1;  // the place's map id; the map the player was in (a change: the travel happened)
    std::vector<int> ids;            // every id the game's list may name the place by
    int list_id = -1;                // the one it does
    bool cancel = false;             // Cancel on the card: close the game's dialog
    int confirm = 0;                 // the game's "travel?" dialog: 1 Up pressed (to Yes), 2 A pressed
    char refused = 0;                // the game said no: E a story event, L a locked map, M not from here (the map type), ? other
    uint64_t start = 0, phase_t = 0, next = 0;
    int presses = 0, tries = 0;
    bool Busy() const { return phase != kIdle; }
};

struct Assets {
    std::mutex mu;
    bool tried = false, ok = false;
    std::string why;
    ffr::IoStore io;
    ffr::Pak pak;
    std::unordered_map<std::string, std::string> loc;
    std::unordered_map<std::string, std::string> quest_keys;  // "<namespace>\x1f<Japanese text>" -> key, for the quest tables (ja locres)
    ffr::Font regular, bold, title;
    std::map<std::string, std::unique_ptr<ffr::Image>> images;  // nullptr value = failed
    EdenDsmodHostApi host{};                                    // the worker's host copy for romfs reads

    bool Ensure(const EdenDsmodHostApi* h) {
        host = *h;
        if (tried) return ok;
        tried = true;
        ffr::RomRead rd = [this](const char* p, uint64_t off, void* out, size_t n) -> size_t {
            return host.read_romfs ? host.read_romfs(host.userdata, p, off, out, n) : 0;
        };
        std::string err;
        if (!io.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.utoc", "romfs:/FFRS/Content/Paks/FFRS-Switch.ucas", err)) { why = "container: " + err; return false; }
        if (!pak.Open(rd, "romfs:/FFRS/Content/Paks/FFRS-Switch.pak", err)) { why = "pak: " + err; return false; }
        std::vector<uint8_t> b, t;
        if (pak.Read("FFRS/Content/Localization/Game/en/Game.locres", b)) ffr::ParseLocres(b, loc);
        // A quest name the game has not looked up yet comes as its Japanese source text (Reader::StringTableSource): the
        // game's ja locres gives its key, and en its English
        std::unordered_map<std::string, std::string> ja;
        if (pak.Read("FFRS/Content/Localization/Game/ja/Game.locres", b) && ffr::ParseLocres(b, ja))
            for (auto& [k, text] : ja) {
                const size_t sep = k.find('\x1f');
                if (sep != std::string::npos && k.find("Quest") < sep)  // (the quest tables only: ST_EventQuestTitle ...)
                    quest_keys.emplace(k.substr(0, sep + 1) + text, k.substr(sep + 1));
            }
        auto font = [&](const char* f, ffr::Font& out) {
            if (pak.Read(std::string("FFRS/Content/UI/Font/FontFace/SE/") + f, b) && ffr::UnwrapFontFace(b, t)) out.Load(t);
        };
        font("SE-VOGEN.ufont", regular);
        font("SE-VOGEB___.ufont", bold);
        font("SE-GARRITHR.ufont", title);
        ok = regular.ok() && bold.ok() && title.ok();
        if (!ok) why = "fonts";
        return ok;
    }
    const ffr::Image* Image(const std::string& path) {
        auto it = images.find(path);
        if (it != images.end()) return it->second.get();
        if (images.size() > 400) images.clear();
        auto im = std::make_unique<ffr::Image>();
        std::vector<uint8_t> b;
        if (!io.Read(path, b) || !ffr::DecodeTexture(b, *im)) {
            im.reset();
            std::lock_guard<std::mutex> lk(failed_mu);
            failed.insert(path);
        }
        return (images[path] = std::move(im)).get();
    }
    // Pictures that could not be read or decoded, for the tick thread (MapAvailable) without taking mu
    std::mutex failed_mu;
    std::unordered_set<std::string> failed;
    bool Failed(const std::string& path) {
        std::lock_guard<std::mutex> lk(failed_mu);
        return failed.count(path) != 0;
    }
};

struct Instance {
    EdenDsmodHostApi host{};
    ffr::Reader reader;
    bool reader_ok = false;
    std::string reader_why;
    Assets assets;
    std::mutex state_mu;
    ffr::GameState state;  // latest, read on the tick thread
    ffr::View view;
    int before_tab = ffr::kTabMap, before_member = -1;  // what was open when the battle took over the screen
    bool map_fallback = false;  // Map was open but this place has no map: Party is shown, and Map comes back with one
    MapFrame frame;             // the map widgets' coordinates for the current place (full read)
    ffr::MapState live;         // the player, read 30 times a second
    bool have_live = false;
    Drive drive;
    int press = kBtnNone;       // the button to press this tick
    MapFrame wframe;            // the world map for the scope switch (in a town or dungeon), the frame itself on the world map
    bool world_view = false;    // World chosen on the scope switch in a town or dungeon
    int card = -1;              // the place whose card shows at the foot of the world map (map id), -1 none
    Travel travel;
    ffr_helper::Client helper;    // the guest helper's mailbox (bound when the package's load plan was installed)
    // A request of the Items tab to the helper (one at a time, never while a fast travel uses the mailbox)
    struct Job {
        enum Kind { kNone, kUse, kEquip, kUnequip, kWearable, kTrack } kind = kNone;
        bool tracked = false;      // kTrack: the quest was the tracked one (the game's Track then untracks it)
        int unit = -1, item = -1, slot = -1;  // slot: the page's (0 weapon, 1 armor, 2 and 3 the accessories)
        int index = 0;             // kEquip: the slot index the game takes (accessories: the place in the member's list)
        std::vector<int> ids;      // kWearable: the pieces asked about
        uint64_t start = 0;
        int hp = 0, mp = 0;        // kUse: the target's before (the change shows on its card)
        std::vector<std::array<int, 3>> before;  // kUse on the party: every member's id, HP, MP
    } job;
    struct Pending {               // a used item: its effect, once the next read shows it
        int unit = -1;
        std::vector<std::array<int, 3>> before;
        uint64_t until = 0;
        int kind = 0;
    } pending;
    std::vector<std::pair<ffr::View::Fx, uint64_t>> fx;  // what just happened, and until when it shows
    // the game's answer for which pieces a member can wear: (unit, the ids asked) -> the ones it can
    int quest_shown = -1;          // the quest shown on the Quests tab (its id: it keeps its place as the list moves)
    int wear_unit = -1;
    std::vector<int> wear_ids, wear_ok;
    bool wear_known = false;
    uint32_t helper_beat = 0;     // its heartbeat as last read, and when it last moved
    uint64_t helper_beat_ms = 0;
    bool helper_logged = false;
    bool HelperAlive(uint64_t now) const { return helper.Bound() && helper_beat_ms && now - helper_beat_ms < 1000; }
    std::vector<uint32_t> snd_ids;  // the game's UI sound tags (FNames, kSndNames), looked up once the helper runs
    bool snd_done = false;
    uint64_t snd_retry_ms = 0;
    uint64_t travel_failed = 0; // until when the card says the game's map did not open
    char travel_refused = 0;    // and why the game refused the last one (Travel::refused), shown until the card closes
    ffr::WorldMenu wm;
    ffr::UiState ui;              // who has the game's controls (read while a card shows or a travel runs)
    uint64_t ui_ms = 0;
    std::vector<std::array<std::string, 3>> icon_keys, goal_keys, arrow_keys, mark_keys, island_keys[kIslandClasses];
    double zoom[2] = {1.0, 1.0};  // the following zoom: town, world
    double view_zoom[2] = {1.0, 1.0};  // the host's own view zoom on top (a pinch), as last read
    int reset = -1;               // Home: the map view the host is to send home on this sample (enforce), -1 none
    uint64_t serial = 0, last_key = 0, map_serial = 0;
    uint32_t ticks = 0;
    std::string page_key;
};

void Log(const EdenDsmodHostApi* h, uint32_t lvl, const std::string& s) {
    if (h && h->log) h->log(h->userdata, lvl, ("ffr: " + s).c_str());
}


// The full NSO build id of update 1.0.1 (16 bytes); the host's 32-byte id is this plus zero padding.
constexpr char kBuildHex[] = "84E182F72EE1DA4F477804BD05B54C15";

bool BuildIdMatches(const char* b) {
    if (!b) return false;
    constexpr size_t kLen = sizeof(kBuildHex) - 1;
    const size_t n = std::strlen(b);
    if (n < kLen || n > 64) return false;
    for (size_t i = 0; i < n; ++i) {
        const char want = i < kLen ? kBuildHex[i] : '0';
        const char c = (b[i] >= 'a' && b[i] <= 'f') ? char(b[i] - 'a' + 'A') : b[i];
        if (c != want) return false;
    }
    return true;
}

EdenDsmodBool SupportsBuild(const char* b) {
    return BuildIdMatches(b) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    auto* in = new Instance();
    in->host = *host;
    char full[65] = {};
    for (int i = 0; i < 32; ++i) std::snprintf(full + 2 * i, 3, "%02X", host->build_id[i]);
    if (BuildIdMatches(full)) {
        in->reader_ok = in->reader.Init(host, full, in->reader_why);  // (the reader's table is keyed by the first 16 digits)
    } else {
        in->reader_why = "unsupported build";
    }
    Log(host, in->reader_ok ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_WARNING, in->reader_ok ? "reader ready" : "reader: " + in->reader_why);
    return in;
}

void Destroy(void* p) {
    auto* in = static_cast<Instance*>(p);
    delete in;
}

bool MapAvailable(Instance* in, const ffr::GameState& s) {  // the Map tab has a picture to show
    const ffr::MapState& m = s.map;
    if (!m.valid) return false;
    if (m.world) return !m.islands.empty();
    return !m.texture.empty() && m.bw > 0 && m.bh > 0 && !in->assets.Failed(m.texture);
}

// One step of a tap's drive; called every tick while one runs (state_mu held)
void StepDrive(Instance* in) {
    Drive& d = in->drive;
    in->press = kBtnNone;
    if (!d.kind) return;
    const uint64_t now = NowMs();
    const ffr::BattleMenu& mn = in->state.menu;
    auto finish = [&](int b) { in->press = b; d = Drive{}; };
    if (now - d.start > 5000 || !in->state.in_battle) { d = Drive{}; return; }  // gave up: the game went elsewhere
    if (now < d.next) return;
    if (!mn.valid) { if (now - d.start > 1500) d = Drive{}; return; }
    auto press = [&](int b) { in->press = b; d.next = now + 140; ++d.presses; };
    switch (d.kind) {
    case 1:  // an entry of the open list
        if (mn.state != 0 || d.want >= int(mn.entries.size()) || !(mn.entries[size_t(d.want)] == d.key) || d.presses > int(mn.entries.size()) + 6) { d = Drive{}; return; }
        if (mn.cursor == d.want) finish(kBtnA);
        else press(mn.cursor < d.want ? kBtnDown : kBtnUp);
        break;
    case 2: {  // a target: when the game's cursor is on it, confirm; otherwise look for it along each direction in turn
        if (mn.state != 1) { d = Drive{}; return; }
        if (std::find(mn.targets.begin(), mn.targets.end(), d.unit) != mn.targets.end()) { finish(kBtnA); return; }
        static const int kDirs[4] = {kBtnDown, kBtnRight, kBtnUp, kBtnLeft};
        const int n = int(in->state.battle.size()) + 1;
        if (d.dir_presses >= n) ++d.dir, d.dir_presses = 0;
        if (d.dir >= 4) { d = Drive{}; return; }
        ++d.dir_presses;
        press(kDirs[d.dir]);
        break;
    }
    case 4: {  // a resonance: its direction, then A if the wheel is still up (the direction may only move the game's pointer)
        static const int kNav[4] = {kBtnLeft, kBtnRight, kBtnUp, kBtnDown};
        if (d.presses == 0) { press(kNav[d.want & 3]); d.next = now + 260; }
        else if (mn.state == 2) finish(kBtnA);
        else d = Drive{};
        break;
    }
    default:  // back
        finish(kBtnB);
        break;
    }
}

// One step of a fast travel (state_mu held); every tick while one runs, after the battle drive (never both: no travel in a battle).
void StepTravel(Instance* in, const EdenDsmodHostApi* host) {
    Travel& t = in->travel;
    if (!t.Busy() || in->press != kBtnNone) return;
    const uint64_t now = NowMs();
    auto go = [&](int ph) { t.phase = ph, t.phase_t = now, t.presses = 0, ++in->map_serial; };  // (a new page: the card's status)
    auto press = [&](int b, int wait) { in->press = b, t.next = now + uint64_t(wait), ++t.presses; };
    auto stop = [&](bool failed) {
        t = Travel{};
        if (failed) in->travel_failed = now + 3500;
        ++in->map_serial;
    };
    if (in->state.in_battle) { stop(false); return; }
    // The travel happened: the player is in another map
    const int map_now = in->reader.PlayerMapId(host);
    if (t.from_map > 0 && map_now > 0 && map_now != t.from_map) {
        stop(false);
        in->world_view = false, in->card = -1;
        return;
    }
    if (now < t.next) return;
    if (t.phase < Travel::kDialog && now - t.start > 30000) { stop(true); return; }
    if (t.phase == Travel::kDirect) {
        auto fallback = [&](const char* why) {  // the game's map, as a player would (the helper could not take it)
            Log(host, EDEN_DSMOD_LOG_INFO, std::string("travel: the helper did not take it (") + why + "), through the game's map");
            go(Travel::kOpenMap);
        };
        if (!t.sent) {
            uint64_t da = 0, cls = 0;
            if (!in->reader.TravelAsset(host, da, cls)) { fallback("no DA_WldMapParameter"); return; }
            if (!in->helper.Stage(ffr_helper::Op::Travel, da, cls, t.target, t.landing)) { fallback("mailbox"); return; }
            t.sent = true, t.phase_t = now;
            return;
        }
        if (!t.answered) {
            const ffr_helper::Outcome o = in->helper.Poll();
            if (!o.answered) {
                if (now - t.phase_t > 1500) { in->helper.Abandon(); fallback("no answer"); }
                return;
            }
            t.answered = true, t.phase_t = now;
            if (o.code == ffr_helper::Result::Ok) {  // the game travels: its fade, then the new map (the check at the top)
                Log(host, EDEN_DSMOD_LOG_INFO, "travel: the game travels to " + std::to_string(t.target));
                return;
            }
            if (o.code == ffr_helper::Result::Refused) {  // the game's own rule: the card says why, as its dialog would
                const std::string d = in->reader.Name(o.tag);
                t.refused = d.find("CanNot.Event") != std::string::npos ? 'E' : d.find("CanNot.LockMap") != std::string::npos ? 'L'
                          : d.find("CanNot.MapType") != std::string::npos ? 'M' : '?';
                in->travel_refused = t.refused;
                Log(host, EDEN_DSMOD_LOG_INFO, "travel: the game refuses " + std::to_string(t.target) + " (" + d + ")");
                t.next = now + 2200;
                ++in->map_serial;
                return;
            }
            fallback(o.code == ffr_helper::Result::NotReady ? "not ready" : o.code == ffr_helper::Result::StaleEpoch ? "stale" : "refused");
            return;
        }
        if (t.refused || now - t.phase_t > 20000) stop(!t.refused);  // (refused: the card keeps the reason until it closes)
        return;
    }
    ffr::WorldMenu& w = in->wm;
    in->reader.ReadWorldMenu(host, w, true);
    ffr::UiState& ui = in->ui;
    in->reader.ReadUi(host, ui);
    // Something else took the controls (a conversation, a story scene, the save screen): stop pressing
    if (t.phase > Travel::kOpenMap && (ui.event || ui.save)) { stop(true); return; }
    auto has = [&](int) {  // the place is in the list (by any of its ids): remember which
        for (int id : t.ids)
            if (std::find(w.list.begin(), w.list.end(), id) != w.list.end()) { t.list_id = id; return true; }
        return false;
    };
    switch (t.phase) {
    case Travel::kOpenMap:  // only from the field: nothing else has the controls
        if (!ui.Free() || w.dialog) { stop(true); return; }
        press(kBtnMinus, 250);
        go(Travel::kOpenList);
        break;
    case Travel::kOpenList:  // the map is open once the System layer has the controls (and no conversation or scene took them)
        if (ui.event || ui.save) { stop(true); return; }
        if (ui.input != "UI.Layer.System") {
            if (now - t.phase_t > 2000) stop(true);
            break;
        }
        press(kBtnZL, 350);
        go(Travel::kWaitList);
        break;
    case Travel::kWaitList:  // ZL again until the list is up (the map may take a moment to open), then give up
        if (w.found && w.list_open && !w.list.empty()) { go(Travel::kCategory); break; }
        if (now - t.phase_t > 650) {
            if (t.tries++ < 2 && ui.input == "UI.Layer.System") { press(kBtnZL, 350); t.phase_t = now; }
            else stop(true);  // the map did not open (another menu, a story scene): nothing more is pressed
        }
        break;
    case Travel::kCategory:  // the list's categories in turn (Town, Dungeon, Shrine) until one holds the place
        if (!w.list_open) { stop(true); return; }
        if (has(t.target)) { go(Travel::kEntry); break; }
        if (t.presses >= 4) { go(Travel::kCloseList); break; }  // in none of them: close the map again
        press(kBtnR, 300);
        break;
    case Travel::kEntry: {  // down the list until the game's focus is the place (the list starts on its first entry)
        if (!w.list_open || !has(t.target)) { go(Travel::kCloseList); break; }
        if (w.focus == t.list_id) { press(kBtnA, 250); go(Travel::kWaitDialog); break; }
        if (t.presses > int(w.list.size()) + 2) { go(Travel::kCloseList); break; }
        const auto at = std::find(w.list.begin(), w.list.end(), w.focus), want = std::find(w.list.begin(), w.list.end(), t.list_id);
        press(at != w.list.end() && at > want ? kBtnUp : kBtnDown, 230);
        break;
    }
    case Travel::kWaitDialog:
        if (w.dialog) { go(Travel::kDialog); break; }
        if (now - t.phase_t > 2500) go(Travel::kCloseList);
        break;
    case Travel::kDialog: {  // the game's dialog for the place
        if (!w.dialog) {  // closed: the travel starts (the map changes; after Yes, give its fade time) or not
            const bool yes = t.confirm == 3;
            go(Travel::kCloseList);
            t.next = now + (yes ? 5000 : 1200);
            break;
        }
        if (t.cancel) { t.cancel = false; press(kBtnB, 500); break; }
        const bool travel = w.can_travel && w.travel_dialog == "UI.Dialog.Box.Map.FastTravel";
        if (travel) {  // "Travel there?": the tap on Fast Travel was the player's yes. Up to Yes (it opens on No), then A.
            if (t.confirm == 0) { t.confirm = 1; t.next = now + 250; break; }  // (its opening animation)
            if (t.confirm == 1) { t.confirm = 2; press(kBtnUp, 250); break; }
            if (t.confirm == 2) { t.confirm = 3; press(kBtnA, 600); break; }
            break;  // (A pressed: the dialog closes as the travel starts)
        }
        // The game refuses (a story event, a locked map, not from here): the card says why; the message closes after a moment
        if (!t.refused) {
            const std::string& d = w.travel_dialog;
            t.refused = d.find("CanNot.Event") != std::string::npos ? 'E' : d.find("CanNot.LockMap") != std::string::npos ? 'L'
                      : d.find("CanNot.MapType") != std::string::npos ? 'M' : '?';
            in->travel_refused = t.refused;
            t.next = now + 2200;
            ++in->map_serial;
            break;
        }
        press(kBtnB, 500);
        break;
    }
    case Travel::kCloseList:
        if (w.dialog) { go(Travel::kDialog); break; }
        if (w.list_open) {
            if (t.presses >= 3) { stop(false); return; }
            press(kBtnB, 550);
            break;
        }
        go(Travel::kCloseMap);
        break;
    case Travel::kCloseMap:  // the map is open while its list keeps a category (None once it closes)
        if (w.category && t.presses < 2) { press(kBtnB, 650); break; }
        stop(false);
        break;
    default:
        stop(false);
    }
}

void AddFx(Instance* in, int unit, int slot, std::string text, bool good, uint64_t ms = 1600) {
    ffr::View::Fx f;
    f.unit = unit, f.slot = slot, f.text = std::move(text), f.good = good;
    in->fx.erase(std::remove_if(in->fx.begin(), in->fx.end(), [&](auto& e) { return e.first.unit == unit && e.first.slot == slot; }), in->fx.end());
    in->fx.push_back({f, NowMs() + ms});
    ++in->map_serial;
}

// The game's own menu sounds, by gameplay tag (CPP_BFL_Sound::PlaySound2DByTag through the helper's sound channel)
enum Snd { kSndCursor, kSndCancel, kSndEquip, kSndUnequip, kSndHeal, kSndError, kSndTrackOn, kSndTrackOff, kSndCount };
const std::vector<std::string> kSndNames = {
    "Sound.UI.Cursor", "Sound.UI.Cancel", "Sound.UI.Item.Equip", "Sound.UI.Item.Unequip", "Sound.UI.Status.Heal", "Sound.UI.Error",
    "Sound.UI.MainMenu.Map.Target.On", "Sound.UI.MainMenu.Map.Target.Off"};

void Play(Instance* in, Snd s) {
    if (size_t(s) < in->snd_ids.size() && in->snd_ids[size_t(s)] && in->HelperAlive(NowMs())) in->helper.Sound(in->snd_ids[size_t(s)]);
}

// The Items tab's requests to the helper (state_mu held): one in flight; its answer becomes the card's or slot's feedback
void StepJobs(Instance* in, const EdenDsmodHostApi* host) {
    using J = Instance::Job;
    J& j = in->job;
    const uint64_t now = NowMs();
    // what a used item did, once the read shows it
    if (in->pending.unit >= 0 || !in->pending.before.empty()) {
        bool shown = false;
        for (auto& [id, hp, mp] : in->pending.before)
            for (auto& u : in->state.units)
                if (u.id == id && (u.hp != hp || u.mp != mp)) {
                    std::string t = u.hp != hp ? (hp <= 0 && u.hp > 0 ? "Revived!" : "+" + std::to_string(u.hp - hp) + " HP")
                                               : "+" + std::to_string(u.mp - mp) + " MP";
                    AddFx(in, id, -1, t, true);
                    shown = true;
                }
        if (shown || now > in->pending.until) {
            if (!shown) AddFx(in, in->pending.unit, -1, "Used", true);
            in->pending = Instance::Pending{};
        }
    }
    if (j.kind == J::kNone) return;
    if (!in->helper.Busy() && j.start == 0) {  // stage it
        ffr_helper::Request r;
        r.a = j.unit;
        if (j.kind == J::kUse) {
            uint64_t act = 0, act_cls = 0, tgt = 0, tgt_cls = 0;
            if (!in->reader.ItemAssets(host, act, act_cls, tgt, tgt_cls)) { AddFx(in, j.unit, -1, "Not now", false); j = J{}; return; }
            r.op = ffr_helper::Op::Item, r.obj = act, r.obj_class = act_cls, r.obj2 = tgt, r.obj2_class = tgt_cls, r.a = j.item, r.b = j.unit;
        } else if (j.kind == J::kTrack) {
            uint64_t sub = 0, cls = 0;
            if (!in->reader.QuestSystem(host, sub, cls)) { AddFx(in, -2, j.item, "Not now", false); j = J{}; return; }
            r.op = ffr_helper::Op::Track, r.obj = sub, r.obj_class = cls, r.a = j.item;
        } else if (j.kind == J::kEquip) {
            r.op = ffr_helper::Op::Equip, r.b = j.item, r.c = j.index;
        } else if (j.kind == J::kUnequip) {
            r.op = ffr_helper::Op::Unequip, r.b = j.item;
        } else {
            r.op = ffr_helper::Op::Equippable, r.b = int(j.ids.size()), r.ids = &j.ids;
        }
        if (!in->helper.Stage(r)) { j = J{}; return; }
        j.start = now;
        ++in->map_serial;
        return;
    }
    if (j.start == 0) return;  // (the mailbox is in use: next tick)
    const ffr_helper::Outcome o = in->helper.Poll();
    if (!o.answered) {
        if (now - j.start > 1500) {
            in->helper.Abandon();
            if (j.kind == J::kTrack) AddFx(in, -2, j.item, "No answer", false);
            else if (j.kind != J::kWearable) AddFx(in, j.unit, j.kind == J::kUse ? -1 : j.slot, "No answer", false);
            Log(host, EDEN_DSMOD_LOG_WARNING, "items: the helper did not answer");
            j = J{};
            ++in->map_serial;
        }
        return;
    }
    const bool ok = o.code == ffr_helper::Result::Ok;
    switch (j.kind) {
    case J::kUse:
        Log(host, EDEN_DSMOD_LOG_INFO, "items: item " + std::to_string(j.item) + " on " + std::to_string(j.unit) + (ok ? ": used" : ": the game refused (" + std::to_string(int(o.code)) + ")"));
        Play(in, ok ? kSndHeal : kSndError);
        if (ok) in->pending.unit = j.unit, in->pending.before = j.before, in->pending.until = now + 1200;
        else AddFx(in, j.unit, -1, o.code == ffr_helper::Result::Refused ? "Can't use it now" : "Not now", false);
        break;
    case J::kEquip:
    case J::kUnequip:
        Log(host, EDEN_DSMOD_LOG_INFO, "items: " + std::string(j.kind == J::kEquip ? "equip " : "remove ") + std::to_string(j.item) + " on " + std::to_string(j.unit) + (ok ? ": done" : ": refused"));
        AddFx(in, j.unit, j.slot, ok ? (j.kind == J::kEquip ? "Equipped" : "Removed") : "Can't wear that", ok);
        Play(in, !ok ? kSndError : j.kind == J::kEquip ? kSndEquip : kSndUnequip);
        in->wear_known = false;  // (who wears what changed: ask again)
        break;
    case J::kTrack:
        Log(host, EDEN_DSMOD_LOG_INFO, "quests: track " + std::to_string(j.item) + (ok ? ": done" : ": refused"));
        Play(in, !ok ? kSndError : j.tracked ? kSndTrackOff : kSndTrackOn);
        AddFx(in, -2, j.item, !ok ? "The game won't track this quest now" : j.tracked ? "No longer tracked" : "Now tracked: the map shows where to go", ok, 2400);
        break;
    default:
        if (ok) {
            in->wear_ok.clear();
            for (size_t i = 0; i < j.ids.size(); ++i) if (in->helper.Bit(i)) in->wear_ok.push_back(j.ids[i]);
            in->wear_unit = j.unit, in->wear_ids = j.ids, in->wear_known = true;
        }
        break;
    }
    j = J{};
    ++in->map_serial;
}

// What the Items tab shows of the game and the helper (state_mu held, every tick)
void ItemsView(Instance* in, const EdenDsmodHostApi* host) {
    ffr::View& v = in->view;
    const uint64_t now = NowMs();
    if ((v.tab == ffr::kTabItems || v.tab == ffr::kTabQuests) && !in->state.in_battle && now - in->ui_ms > 150) {  // who has the game's controls
        in->ui_ms = now;
        in->reader.ReadUi(host, in->ui);
    }
    const int act = in->state.in_battle ? ffr::View::kActBattle : !in->HelperAlive(now) ? ffr::View::kActNoHelper
                  : !in->ui.Quiet() ? ffr::View::kActBusy : ffr::View::kActNow;
    const int quest_act = act == ffr::View::kActNow && !in->ui.Free() ? ffr::View::kActBusy : act;
    if (act != v.act || quest_act != v.quest_act) v.act = act, v.quest_act = quest_act, ++in->map_serial;
    // the feedback that is still showing
    const size_t before = in->fx.size();
    in->fx.erase(std::remove_if(in->fx.begin(), in->fx.end(), [&](auto& e) { return e.second < now; }), in->fx.end());
    if (in->fx.size() != before) ++in->map_serial;
    v.fx.clear();
    for (auto& e : in->fx) v.fx.push_back(e.first);
    const auto& j = in->job;
    v.busy_unit = j.kind == Instance::Job::kTrack ? -2 : j.kind == Instance::Job::kUse || j.kind == Instance::Job::kEquip || j.kind == Instance::Job::kUnequip ? j.unit : -1;
    v.busy_slot = j.kind == Instance::Job::kUse ? -1 : j.slot;
    // Equipment: the game's answer for this member and slot, asked again whenever the pieces or the member change
    const auto roster = ffr::Roster(in->state);
    v.eq_member = roster.empty() ? 0 : std::clamp(v.eq_member, 0, int(roster.size()) - 1);
    v.wearable_known = false;
    v.wearable.clear();
    if (v.tab == ffr::kTabItems && v.item_cat == 1 && !roster.empty()) {
        const int unit = roster[size_t(v.eq_member)]->id;
        const auto ids = ffr::EquipChoices(in->state, v);
        if (in->wear_known && in->wear_unit != unit) in->wear_known = false;  // another member: ask the game for them
        if (in->wear_known) {
            for (int id : ids) if (std::find(in->wear_ok.begin(), in->wear_ok.end(), id) != in->wear_ok.end()) v.wearable.push_back(id);
            // every piece asked about must be among those known (a new piece picked up: ask again)
            bool all = true;
            for (int id : ids) all = all && std::find(in->wear_ids.begin(), in->wear_ids.end(), id) != in->wear_ids.end();
            if (all) v.wearable_known = true;
            else in->wear_known = false;
        }
        if (!in->wear_known && act == ffr::View::kActNow && in->job.kind == Instance::Job::kNone && !in->travel.Busy()) {
            // ask for every piece the player has (all slot types at once: one answer covers the member's slots)
            ffr::View all = v;
            std::vector<int> every;
            for (int sl = 0; sl < 3; ++sl) { all.eq_slot = sl; auto x = ffr::EquipChoices(in->state, all); every.insert(every.end(), x.begin(), x.end()); }
            if (every.size() > ffr_helper::kMaxIds) every.resize(ffr_helper::kMaxIds);
            in->job = Instance::Job{};
            in->job.kind = Instance::Job::kWearable, in->job.unit = unit, in->job.ids = every;
        }
    }
    if (v.tab == ffr::kTabItems && v.item_cat == 1) {
        const int rows = ffr::ListRows(v), n = ffr::ListCount(in->state, v);
        v.eq_scroll = std::max(0, std::min(v.eq_scroll, n - rows));
    }
    // Quests: the shown quest stays the same one as the list changes order (tracking moves it to the top)
    const auto quests = ffr::QuestsOf(in->state, v.quest_cat);
    if (in->quest_shown > 0) {
        int at = -1;
        for (size_t i = 0; i < quests.size(); ++i) if (quests[i]->id == in->quest_shown) at = int(i);
        v.quest_sel = at;
    }
    if (v.quest_sel >= int(quests.size())) v.quest_sel = -1;
    v.quest_scroll = std::max(0, std::min(v.quest_scroll, int(quests.size()) - ffr::kQuestRows));
}

uint64_t ViewKey(const ffr::View& v) {
    uint64_t k = uint64_t(v.map_ok) << 60 | uint64_t(v.tab) | uint64_t(v.member + 1) << 4 | uint64_t(v.item_cat) << 12 | uint64_t(v.item_scroll) << 16 | uint64_t(v.item_sel + 1) << 32;
    auto mix = [&](uint64_t x) { k = (k ^ x) * 1099511628211ull; };
    mix(uint64_t(v.eq_member)), mix(uint64_t(v.eq_slot)), mix(uint64_t(v.eq_scroll)), mix(uint64_t(v.act)), mix(uint64_t(v.quest_act)), mix(uint64_t(v.wearable_known));
    mix(uint64_t(v.busy_unit + 1)), mix(uint64_t(v.busy_slot + 1)), mix(uint64_t(v.quest_cat)), mix(uint64_t(v.quest_scroll)), mix(uint64_t(v.quest_sel + 1));
    for (int id : v.wearable) mix(uint64_t(id));
    for (auto& f : v.fx) { mix(uint64_t(f.unit)), mix(uint64_t(f.slot + 1)); for (char ch : f.text) mix(uint64_t(uint8_t(ch))); }
    return k;
}

void NewPageIfChanged(Instance* in) {  // state_mu held
    const uint64_t key = (in->state.Hash() * 31 + ViewKey(in->view)) * 31 + in->map_serial;
    if (key != in->last_key || in->page_key.empty()) {
        in->last_key = key;
        in->page_key = kPrefix + std::to_string(++in->serial);
    }
}

void Sample(void* p, const EdenDsmodHostApi* host) {
    auto* in = static_cast<Instance*>(p);
    in->host.userdata = host->userdata;
    ++in->ticks;
    if (in->ticks % 6 == 1) {  // a full read ~10 times a second
        ffr::GameState s;
        if (in->reader_ok) in->reader.Update(host, s);
        else s.status = "Unsupported game version";
        std::lock_guard<std::mutex> lk(in->state_mu);
        ffr::View& v = in->view;
        if (s.in_game && s.save_ok && !(in->state.in_game && in->state.save_ok)) v = ffr::View{};  // a game was loaded: start on Party
        if (s.in_battle && !in->state.in_battle) {  // battle starts: follow it, and remember what was open
            in->before_tab = v.tab, in->before_member = v.member;
            v.tab = ffr::kTabBattle;
            v.member = -1;
        }
        if (!s.in_battle && in->state.in_battle && v.tab == ffr::kTabBattle) {  // battle over: back to that page
            v.tab = in->before_tab;
            v.member = in->before_member;
        }
        // keep the selection valid as the data changes
        auto roster = ffr::Roster(s);
        if (v.member >= int(roster.size())) v.member = -1;
        const int n = int(ffr::ItemsOf(s, v.item_cat).size());
        if (v.item_sel >= n) v.item_sel = -1;
        v.item_scroll = std::max(0, std::min(v.item_scroll, n - ffr::kItemRows));
        // No map picture for this place: the Map tab is dimmed and closed, and an open Map tab moves to Party until there is one
        v.map_ok = MapAvailable(in, s);
        if (!v.map_ok && v.tab == ffr::kTabMap) v.tab = ffr::kTabParty, v.member = -1, in->map_fallback = true;
        if (v.map_ok && in->map_fallback && v.tab == ffr::kTabParty && v.member < 0) v.tab = ffr::kTabMap, in->map_fallback = false;
        in->frame = Frame(s.map);
        in->wframe = WorldFrame(s.map);
        if (s.map.map_id != in->state.map.map_id) in->world_view = false, in->card = -1;  // a new place: its own map first
        if (in->wframe.mode != 2 || in->frame.mode == 2) in->world_view = false;
        in->state = std::move(s);
        NewPageIfChanged(in);
    } else if (in->ticks % 2 == 1 && in->reader_ok) {  // in a battle, HP, MP and the command cursor in between: ~30 times a second
        std::lock_guard<std::mutex> lk(in->state_mu);
        if (in->state.in_battle && in->reader.RefreshBattle(host, in->state) && in->reader.RefreshMenu(host, in->state)) NewPageIfChanged(in);
    }
    std::lock_guard<std::mutex> lk(in->state_mu);
    StepDrive(in);
    if (in->helper.Bound()) {  // the guest helper ticks with the game: its heartbeat says it runs
        const uint64_t now = NowMs();
        const uint32_t beat = in->helper.Heartbeat();
        if (beat != in->helper_beat) {
            in->helper_beat = beat, in->helper_beat_ms = now;
            if (!in->helper_logged) { in->helper_logged = true; Log(host, EDEN_DSMOD_LOG_INFO, "helper: running (fast travel through the game's own code)"); }
        }
        if (in->snd_done && now > in->snd_retry_ms &&
            std::find(in->snd_ids.begin(), in->snd_ids.end(), 0u) != in->snd_ids.end())
            in->snd_done = false, in->snd_ids.clear();  // (one was not in the pool yet: look again)
        if (!in->snd_done && in->HelperAlive(now) && in->reader.FindNames(host, kSndNames, in->snd_ids)) {
            in->snd_done = true, in->snd_retry_ms = now + 60000;
            const auto found = std::count_if(in->snd_ids.begin(), in->snd_ids.end(), [](uint32_t i) { return i != 0; });
            Log(host, EDEN_DSMOD_LOG_INFO, "helper: " + std::to_string(found) + " of " + std::to_string(kSndNames.size()) + " menu sounds found");
        }
    }
    StepTravel(in, host);
    StepJobs(in, host);
    ItemsView(in, host);
    NewPageIfChanged(in);
    for (int b = 0; b < kBtnCount; ++b) host->publish_i64(host->userdata, kBtnPoints[b], in->press == b);
    // Home: the host sends that map view home on this sample (its enforce rule, gated like the presses)
    host->publish_i64(host->userdata, "ffr.map.town_reset", in->reset == 0);
    host->publish_i64(host->userdata, "ffr.map.world_reset", in->reset == 1);
    host->publish_i64(host->userdata, "ffr.drv.pending", in->press != kBtnNone || in->reset >= 0);
    in->reset = -1;
    host->publish_i64(host->userdata, "ffr.battle_menu", in->view.tab == ffr::kTabBattle && in->state.in_battle);
    // The live map: the frame's statics and the player, every tick (the host keeps nothing between samples)
    // In a town or dungeon the scope switch may show the world instead (its own frame, the player at the place they are in)
    const bool show_world = in->world_view && in->frame.mode == 1 && in->wframe.mode == 2;
    const MapFrame& f = show_world ? in->wframe : in->frame;
    const ffr::MapState& ms = in->state.map;
    const ffr::Landmark* here = ms.here >= 0 && ms.here < int(ms.landmarks.size()) ? &ms.landmarks[size_t(ms.here)] : nullptr;
    const bool on_map = in->view.tab == ffr::kTabMap && in->view.map_ok && in->state.in_game && in->state.save_ok && f.mode;
    host->publish_i64(host->userdata, "ffr.map.town", on_map && f.mode == 1);
    host->publish_i64(host->userdata, "ffr.map.world", on_map && f.mode == 2);
    if (on_map) {
        // The player 30 times a second; the last position is published on every tick, as the host keeps nothing between samples
        if (in->ticks % 2 == 0 && in->reader_ok && in->reader.ReadPlayer(host, in->live)) in->have_live = true;
        if (show_world) {  // the player at their town on the world map (off the map when it is not known)
            host->publish_f64(host->userdata, "ffr.map.px", here ? f.X(here->x) : -1e7);
            host->publish_f64(host->userdata, "ffr.map.py", here ? f.Y(here->y) : -1e7);
            host->publish_f64(host->userdata, "ffr.map.yaw", 0.0);
        } else if (in->have_live) {
            host->publish_f64(host->userdata, "ffr.map.px", f.X(in->live.px));
            host->publish_f64(host->userdata, "ffr.map.py", f.Y(in->live.py));
            host->publish_f64(host->userdata, "ffr.map.yaw", in->live.yaw);
        }
        if (f.mode == 1) host->publish_text(host->userdata, "ffr.map.base", f.base.c_str());
        // The window the map shows: around the player at the map's own zoom (+/-, Home), centred on them. The host holds a
        // dragged or pinched view where it was left; Home sends it back here.
        const int kind = f.mode == 2 ? 1 : 0;
        const double z = in->zoom[kind], aspect = double(ffr::kMapW) / ffr::kMapH;
        double hw;  // half the window's width
        if (f.mode == 2) {
            hw = 13000 * f.k / z;  // about as close as the game's own map opens at zoom 1
        } else {  // a town: as wide as the whole picture fitted into the widget at zoom 1, closer in above it
            const double tw = f.v[2] - f.v[0], th = f.v[3] - f.v[1];
            hw = (tw / th > aspect ? tw : th * aspect) / 2 / z;
        }
        const double hh = hw / aspect;
        // centred on the player, always, even at the picture's edge (the map moves under the marker)
        double cx = (f.v[0] + f.v[2]) / 2, cy = (f.v[1] + f.v[3]) / 2;
        if (show_world) {
            cx = here ? f.X(here->x) : f.n / 2, cy = here ? f.Y(here->y) : f.n / 2;
        } else if (in->have_live) {
            cx = f.X(in->live.px), cy = f.Y(in->live.py);
        }
        const double v[4] = {cx - hw, cy - hh, cx + hw, cy + hh};
        static const char* kView[4] = {"ffr.map.vx0", "ffr.map.vy0", "ffr.map.vx1", "ffr.map.vy1"};
        if (f.mode == 1 || in->have_live || show_world)
            for (int i = 0; i < 4; ++i) host->publish_f64(host->userdata, kView[i], v[i]);
        // Has the player moved the view (the host's own test, view_custom)? Then +/- zoom that view, not the following one.
        {
            const std::string w = kMapWidget[kind];
            const double vz = host->get_f64 ? host->get_f64(host->userdata, ("@view_zoom:" + w).c_str(), 1.0) : 1.0;
            const double vpx = host->get_f64 ? host->get_f64(host->userdata, ("@view_pan_x:" + w).c_str(), 0.0) : 0.0;
            const double vpy = host->get_f64 ? host->get_f64(host->userdata, ("@view_pan_y:" + w).c_str(), 0.0) : 0.0;
            const bool look = std::fabs(vz - 1.0) > 0.001 || std::fabs(vpx) > 0.5 || std::fabs(vpy) > 0.5;
            if (std::isfinite(vz) && vz > 0) in->view_zoom[kind] = vz;
            host->publish_i64(host->userdata, kind ? "ffr.map.world_follow" : "ffr.map.town_follow", !look);
            host->publish_i64(host->userdata, kind ? "ffr.map.world_look" : "ffr.map.town_look", look);
        }
        // An arrow at the edge of the map for each objective out of view, in its colour, pointing the way. The view as the host
        // last drew it (one tick behind): its centre and px per map unit.
        std::vector<MapFrame::Pin> arrows;
        const std::string a = kMapArea[kind];
        const double mcx = host->get_f64 ? host->get_f64(host->userdata, ("@map_view_cx:" + a).c_str(), NAN) : NAN;
        const double mcy = host->get_f64 ? host->get_f64(host->userdata, ("@map_view_cy:" + a).c_str(), NAN) : NAN;
        const double ppw = host->get_f64 ? host->get_f64(host->userdata, ("@map_view_ppw:" + a).c_str(), NAN) : NAN;
        if (std::isfinite(mcx) && std::isfinite(mcy) && std::isfinite(ppw) && ppw > 0) {
            const double halfw = ffr::kMapW / 2.0, halfh = ffr::kMapH / 2.0;
            for (auto& g : f.goal) {
                const double dx = (g.x - mcx) * ppw, dy = (mcy - g.y) * ppw;  // px from the centre on screen (the map's y is up)
                if (std::fabs(dx) < halfw - 12 && std::fabs(dy) < halfh - 12) continue;  // in view: its own marker shows it
                // on the edge, inset by the arrow's size (and on the right, clear of the zoom buttons)
                const double ex = dx > 0 ? halfw - 52 - 104 : halfw - 52, ey = halfh - 52;
                const double t = std::min(ex / std::max(std::fabs(dx), 1e-6), ey / std::max(std::fabs(dy), 1e-6));
                const double deg = std::atan2(dy, dx) * 180 / M_PI;  // 0 = right, clockwise on screen
                const int bucket = (int(std::lround(deg / 15)) % 24 + 24) % 24;
                arrows.push_back({mcx + dx * t / ppw, mcy - dy * t / ppw,
                                  std::string(kKeys) + "arrow:" + (g.main && g.cls ? "b" : g.main ? "m" : "s") + ":" + std::to_string(bucket), 0, g.main});
            }
            // the gold ones drawn last, over the others
            std::stable_sort(arrows.begin(), arrows.end(), [](const MapFrame::Pin& p, const MapFrame::Pin& q) { return p.main < q.main; });
        }
        auto pins = [&](const std::vector<MapFrame::Pin>& v, std::vector<std::array<std::string, 3>>& keys, const char* stem, int max) {
            if (keys.empty())
                for (int i = 0; i < max; ++i) {
                    const std::string b = std::string("ffr.map.") + stem + std::to_string(i);
                    keys.push_back({b + ".x", b + ".y", b + ".src"});
                }
            for (size_t i = 0; i < v.size() && i < keys.size(); ++i) {
                host->publish_f64(host->userdata, keys[i][0].c_str(), v[i].x);
                host->publish_f64(host->userdata, keys[i][1].c_str(), v[i].y);
                host->publish_text(host->userdata, keys[i][2].c_str(), v[i].src.c_str());
            }
        };
        if (!show_world) {  // (a town's own icons and objectives are in its coordinates)
            pins(f.icons, in->icon_keys, "icon", kMaxIcons);
            std::vector<MapFrame::Pin> goals(f.goal.rbegin(), f.goal.rend());  // the gold ones (first) drawn last
            pins(goals, in->goal_keys, "goal", kMaxGoals);
            pins(arrows, in->arrow_keys, "arrow", kMaxGoals);
        }
        if (f.mode == 2) {  // the places: where the player is (gold), where they can travel (bright), the others (dim)
            std::vector<MapFrame::Pin> marks = f.marks;
            for (size_t i = 0; i < marks.size(); ++i) {
                const int id = f.mark_ids[i];
                const ffr::Landmark* l = nullptr;
                for (auto& x : ms.landmarks) if (x.map_id == id) l = &x;
                const char st = l && here && l->map_id == here->map_id ? 'h' : l && l->CanTravel() ? 't' : 'd';
                const char ic = l && l->type == 1 ? 'c' : l && l->type == 4 ? 's' : 't';  // the game's icon: cave, shrine, town
                marks[i].src = std::string(kKeys) + "lm:" + st + (id == in->card ? "1" : "0") + ic;
            }
            pins(marks, in->mark_keys, "lm", kMaxMarks);
        }
        for (int c = 0; c < kIslandClasses; ++c) {
            std::vector<MapFrame::Pin> v;
            for (auto& p : f.islands)
                if (p.cls == c) v.push_back(p);
            pins(v, in->island_keys[c], ("isl" + std::to_string(c) + "_").c_str(), kIslandsPerClass);
        }
    }
    // The scope switch (World > this place) in a town or dungeon with a world map to show, Back while the world shows, and the
    // card of the place tapped on the world map (its fast travel and how it goes)
    {
        const bool scope = on_map && in->frame.mode == 1 && in->wframe.mode == 2;
        const std::string place = ms.name.empty() ? "Here" : ms.name;
        host->publish_i64(host->userdata, "ffr.map.scope", scope);
        host->publish_i64(host->userdata, "ffr.map.back", scope && show_world);
        if (scope) {
            host->publish_text(host->userdata, "ffr.map.scope_src", (std::string(kKeys) + "scope:" + (show_world ? "1:" : "0:") + place).c_str());
            host->publish_text(host->userdata, "ffr.map.back_src", (std::string(kKeys) + "back:" + place).c_str());
        }
        const ffr::Landmark* cl = nullptr;
        for (auto& x : ms.landmarks) if (x.map_id == in->card) cl = &x;
        const bool card = on_map && f.mode == 2 && cl;
        const uint64_t now = NowMs();
        if (card && !in->travel.Busy() && now - in->ui_ms > 150) {  // is the player free in the field? (the button follows it)
            in->ui_ms = now;
            const bool was = in->ui.Free();
            in->reader.ReadUi(host, in->ui);
            if (was != in->ui.Free()) ++in->map_serial;
        }
        char st = 0;
        if (card) {
            const Travel& t = in->travel;
            if (t.Busy() && t.target == cl->map_id)
                st = t.refused ? 'R' : t.phase >= Travel::kCloseList ? 'C' : 'O';
            else if (in->travel_failed > now && in->ui.Free()) st = 'F';
            else if (in->travel_refused) st = 'R';
            else if (here && here->map_id == cl->map_id) st = 'H';
            else if (!cl->visited) st = 'V';
            else if (ms.map_type == 1) st = 'D';
            else if (!in->ui.Free()) st = 'B';
            else st = 'T';
        }
        host->publish_i64(host->userdata, "ffr.map.card", card);
        host->publish_i64(host->userdata, "ffr.map.card_go", card && (st == 'T' || st == 'F') && !in->travel.Busy());
        host->publish_i64(host->userdata, "ffr.map.card_cancel", card && st == 'O');
        if (card) {
            const char* kind = cl->type == 0 ? "Town" : cl->type == 1 ? "Dungeon" : cl->type == 4 ? "Shrine" : cl->type == 2 ? "Field" : "Place";
            const char why = in->travel.Busy() ? in->travel.refused : in->travel_refused;
            host->publish_text(host->userdata, "ffr.map.card_src", (std::string(kKeys) + "card:" + st + (st == 'R' ? std::string(1, why ? why : '?') : "") + ":" + kind + ":" + cl->name).c_str());
        }
    }
    host->publish_text(host->userdata, "ffr.page", in->page_key.c_str());
    const int tab = in->view.tab;
    host->publish_i64(host->userdata, "ffr.tab", tab);
    host->publish_i64(host->userdata, "ffr.in_game", in->state.in_game && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.map_tab", in->state.in_game && in->state.save_ok && in->view.map_ok && !in->state.in_battle);
    host->publish_i64(host->userdata, "ffr.party_list", tab == ffr::kTabParty && in->view.member < 0 && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.party_detail", tab == ffr::kTabParty && in->view.member >= 0);
    host->publish_i64(host->userdata, "ffr.items", tab == ffr::kTabItems && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.quests", tab == ffr::kTabQuests && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.tabs", in->state.in_game && !in->state.in_battle);  // a battle takes the whole screen
    host->publish_i64(host->userdata, "ffr.in_battle", in->state.in_battle);
}

void Tick(void*, const EdenDsmodHostApi*) {}

EdenDsmodBool OnAction(void* p, const char* action, int64_t arg) {
    auto* in = static_cast<Instance*>(p);
    std::lock_guard<std::mutex> lk(in->state_mu);
    ffr::View& v = in->view;
    const std::string a = action ? action : "";
    if (a == "tab") {
        if (arg < 0 || arg >= ffr::kTabCount) return EDEN_DSMOD_FALSE;
        if (arg == ffr::kTabMap && !v.map_ok) return EDEN_DSMOD_FALSE;  // no map picture here (MapAvailable)
        if (int(arg) != v.tab) Play(in, kSndCursor);
        v.tab = int(arg);
        v.member = -1;
        in->map_fallback = false;
    } else if (a == "btap") {  // the battle page: the tap point packed as x << 32 | y, relative to the page's content
        const int x = int(int32_t(uint64_t(arg) >> 32)), y = int(int32_t(uint64_t(arg) & 0xffffffffu)) + ffr::kBattleY;
        const ffr::GameState& s = in->state;
        const ffr::BattleTap t = ffr::BattleHit(s, x, y);
        const ffr::BattleMenu& mn = s.menu;
        Drive d;
        d.start = NowMs();
        if (t.kind == ffr::BattleTap::kBack && mn.valid) d.kind = 3;
        else if (t.kind == ffr::BattleTap::kEntry && mn.valid && mn.state == 0) d.kind = 1, d.want = t.index, d.key = mn.entries[size_t(t.index)];
        else if (t.kind == ffr::BattleTap::kUnit && mn.valid && mn.state == 1 && s.battle[size_t(t.index)].unit) d.kind = 2, d.unit = s.battle[size_t(t.index)].unit;
        else if (t.kind == ffr::BattleTap::kFinish && mn.valid && mn.state == 2) d.kind = 4, d.want = mn.finish[size_t(t.index)].dir;
        else return EDEN_DSMOD_FALSE;  // nothing the game would take now
        in->drive = d;
        return EDEN_DSMOD_TRUE;
    } else if (a == "zoom") {  // the map's following zoom: in (> 0) or out, refused at the limit
        const int kind = in->frame.mode == 2 || (in->world_view && in->frame.mode == 1) ? 1 : 0;
        const double z = std::clamp(in->zoom[kind] * (arg > 0 ? kZoomStep : 1 / kZoomStep), kZoomMin[kind], kZoomMax[kind]);
        if (std::fabs(z - in->zoom[kind]) < 1e-9 || !in->frame.mode) return EDEN_DSMOD_FALSE;
        in->zoom[kind] = z;
        ++in->map_serial;  // a new page: the host redraws the map at the new window now, not at the player's next step
    } else if (a == "scope") {  // the scope switch: 1 the world, 0 the town or dungeon the player is in
        const bool world = arg == 1;
        if (in->frame.mode != 1 || in->wframe.mode != 2 || world == in->world_view) return EDEN_DSMOD_FALSE;
        in->world_view = world;
        in->card = -1;
        in->reset = world ? 1 : 0;  // the shown map's view home: on the player's place
        ++in->map_serial;
    } else if (a == "mark") {  // a place tapped on the world map: its card (again: closed); -1 closes it
        const MapFrame& f = in->world_view && in->frame.mode == 1 ? in->wframe : in->frame;
        const int id = arg >= 0 && arg < int(f.mark_ids.size()) ? f.mark_ids[size_t(arg)] : -1;
        if (in->travel.Busy()) return EDEN_DSMOD_FALSE;  // the card shows the travel until it is over
        in->card = id == in->card ? -1 : id;
        in->travel_refused = 0;
        ++in->map_serial;
    } else if (a == "travel") {  // Fast Travel on the card
        const ffr::MapState& m = in->state.map;
        const ffr::Landmark* l = nullptr;
        for (auto& x : m.landmarks) if (x.map_id == in->card) l = &x;
        const bool at = m.here >= 0 && m.here < int(m.landmarks.size()) && l && m.landmarks[size_t(m.here)].map_id == l->map_id;
        if (!l || !l->CanTravel() || at || in->travel.Busy() || in->state.in_battle || in->drive.kind) return EDEN_DSMOD_FALSE;
        if (m.map_type == 1) return EDEN_DSMOD_FALSE;  // no fast travel from a dungeon (the game's rule)
        Travel& t = in->travel;
        t = Travel{};
        if (!in->ui.Free()) return EDEN_DSMOD_FALSE;  // a menu, a conversation or a scene has the controls
        const uint64_t now = NowMs();
        t.phase = in->HelperAlive(now) && !in->helper.Busy() ? Travel::kDirect : Travel::kOpenMap;
        t.target = l->map_id, t.from_map = m.map_id, t.ids = l->ids, t.landing = l->landing;
        t.start = t.phase_t = t.next = now;
        in->travel_failed = 0, in->travel_refused = 0;
        ++in->map_serial;
    } else if (a == "travel_cancel") {  // Cancel on the card: close the game's dialog, or stop and close the map
        Travel& t = in->travel;
        if (!t.Busy()) return EDEN_DSMOD_FALSE;
        if (t.phase == Travel::kDirect) {  // before the helper's answer only: once the game travels, it does
            if (t.answered) return EDEN_DSMOD_FALSE;
            in->helper.Abandon();
            t = Travel{};
            ++in->map_serial;
            return EDEN_DSMOD_TRUE;
        }
        if (t.phase == Travel::kDialog) t.cancel = true;
        else if (t.phase < Travel::kCloseList) t.phase = Travel::kCloseList, t.presses = 0;
        ++in->map_serial;
    } else if (a == "home") {  // back to the player, at the zoom on screen: the module's times the host's (a pinch)
        if (!in->frame.mode) return EDEN_DSMOD_FALSE;
        const bool wv = in->world_view && in->frame.mode == 1;  // the world from a town: centred on the town again, at the zoom on screen
        const int kind = in->frame.mode == 2 || wv ? 1 : 0;
        in->zoom[kind] = std::clamp(in->zoom[kind] * in->view_zoom[kind], kZoomMin[kind], kZoomMax[kind]);
        in->reset = kind;
        ++in->map_serial;
    } else if (a == "member") {
        if (arg < 0 || arg >= int(ffr::Roster(in->state).size())) return EDEN_DSMOD_FALSE;
        v.member = int(arg);
        Play(in, kSndCursor);
    } else if (a == "back") {
        v.member = -1;
        Play(in, kSndCancel);
    } else if (a == "cat") {
        if (arg < 0 || arg > 3) return EDEN_DSMOD_FALSE;
        if (int(arg) != v.item_cat) Play(in, kSndCursor);
        v.item_cat = int(arg);
        v.item_scroll = 0;
        v.item_sel = -1;
    } else if (a == "itap") {  // the Items tab: the tap point packed as x << 32 | y, relative to the area below the categories
        const int x = int(int32_t(uint64_t(arg) >> 32)), y = int(int32_t(uint64_t(arg) & 0xffffffffu)) + ffr::kItemListY;
        const ffr::ItemsTap t = ffr::ItemsHit(in->state, v, x, y);
        using T = ffr::ItemsTap;
        const auto roster = ffr::Roster(in->state);
        switch (t.kind) {
        case T::kRow:
            v.item_sel = v.item_sel == t.index ? -1 : t.index;
            Play(in, v.item_sel < 0 ? kSndCancel : kSndCursor);
            break;
        case T::kTarget: {  // a recovery item on this member: the game uses it (its own check first)
            const auto items = ffr::ItemsOf(in->state, 0);
            if (v.item_sel < 0 || v.item_sel >= int(items.size()) || v.act != ffr::View::kActNow) return EDEN_DSMOD_FALSE;
            const int item = items[size_t(v.item_sel)].id;
            const auto ci = in->state.consumables.find(item);
            const ffr::UnitState* u = nullptr;
            for (auto& m : in->state.units) if (m.id == t.id) u = &m;
            if (!u || ci == in->state.consumables.end() || !ffr::UseTargetOk(*u, ci->second)) { Play(in, kSndError); return EDEN_DSMOD_FALSE; }
            if (in->job.kind != Instance::Job::kNone && in->job.kind != Instance::Job::kWearable) return EDEN_DSMOD_FALSE;
            if (in->job.kind == Instance::Job::kWearable && in->job.start) return EDEN_DSMOD_FALSE;  // (one frame: tap again)
            in->job = Instance::Job{};
            in->job.kind = Instance::Job::kUse, in->job.unit = t.id, in->job.item = item;
            for (const ffr::UnitState* m : ffr::UseTargets(in->state))
                if (ci->second.party || m->id == t.id) in->job.before.push_back({m->id, m->hp, m->mp});
            break;
        }
        case T::kMember:
            if (t.index == v.eq_member) return EDEN_DSMOD_FALSE;
            v.eq_member = t.index, v.eq_scroll = 0;
            Play(in, kSndCursor);
            break;
        case T::kSlot:
            if (t.index == v.eq_slot) return EDEN_DSMOD_FALSE;
            v.eq_slot = t.index, v.eq_scroll = 0;
            Play(in, kSndCursor);
            break;
        case T::kChoice:
        case T::kRemove: {
            if (v.act != ffr::View::kActNow || roster.empty() || (in->job.kind != Instance::Job::kNone && in->job.kind != Instance::Job::kWearable)) return EDEN_DSMOD_FALSE;
            if (in->job.kind == Instance::Job::kWearable && in->job.start) return EDEN_DSMOD_FALSE;
            const ffr::UnitState& u = *roster[size_t(std::clamp(v.eq_member, 0, int(roster.size()) - 1))];
            const int now_id = ffr::SlotItem(u, v.eq_slot);
            if (t.kind == T::kChoice && t.id == now_id) return EDEN_DSMOD_FALSE;  // (already worn there)
            if (t.kind == T::kChoice && (!v.wearable_known || std::find(v.wearable.begin(), v.wearable.end(), t.id) == v.wearable.end()))
                return EDEN_DSMOD_FALSE;  // (only what the game says this member can wear)
            in->job = Instance::Job{};
            in->job.unit = u.id, in->job.slot = v.eq_slot;
            if (t.kind == T::kRemove) in->job.kind = Instance::Job::kUnequip, in->job.item = now_id;
            else in->job.kind = Instance::Job::kEquip, in->job.item = t.id;
            // an accessory goes in its slot: the place in the member's list (the game appends past its end)
            in->job.index = ffr::SlotType(v.eq_slot) == 3 ? std::min<int>(v.eq_slot - 2, int(u.accessories.size())) : 0;
            break;
        }
        default:
            return EDEN_DSMOD_FALSE;
        }
        ++in->map_serial;
    } else if (a == "qtap") {  // the Quests tab: the tap point packed as x << 32 | y, relative to the area below the header
        const int x = int(int32_t(uint64_t(arg) >> 32)), y = int(int32_t(uint64_t(arg) & 0xffffffffu)) + ffr::kQuestCatY;
        const ffr::QuestsTap t = ffr::QuestsHit(in->state, v, x, y);
        const auto quests = ffr::QuestsOf(in->state, v.quest_cat);
        switch (t.kind) {
        case ffr::QuestsTap::kCat:
            if (t.index == v.quest_cat) return EDEN_DSMOD_FALSE;
            v.quest_cat = t.index, v.quest_scroll = 0, v.quest_sel = -1, in->quest_shown = -1;
            Play(in, kSndCursor);
            break;
        case ffr::QuestsTap::kRow:
            v.quest_sel = v.quest_sel == t.index ? -1 : t.index;
            Play(in, v.quest_sel < 0 ? kSndCancel : kSndCursor);
            in->quest_shown = v.quest_sel >= 0 ? quests[size_t(v.quest_sel)]->id : -1;
            break;
        case ffr::QuestsTap::kTrack: {  // the journal's Track (the tracked quest again: untrack)
            if (v.quest_act != ffr::View::kActNow || v.quest_sel < 0 || v.quest_sel >= int(quests.size())) return EDEN_DSMOD_FALSE;
            if (in->job.kind != Instance::Job::kNone && (in->job.kind != Instance::Job::kWearable || in->job.start)) return EDEN_DSMOD_FALSE;
            in->job = Instance::Job{};
            in->job.kind = Instance::Job::kTrack, in->job.item = quests[size_t(v.quest_sel)]->id, in->job.tracked = quests[size_t(v.quest_sel)]->tracked;
            break;
        }
        default:
            return EDEN_DSMOD_FALSE;
        }
        ++in->map_serial;
    } else if (a == "scroll") {
        const int n = ffr::ListCount(in->state, v), rows = ffr::ListRows(v);
        int& first = v.tab == ffr::kTabQuests ? v.quest_scroll : v.item_cat == 1 ? v.eq_scroll : v.item_scroll;
        const int s = std::max(0, std::min(first + int(arg) * (rows - 1), n - rows));
        if (s == first) return EDEN_DSMOD_FALSE;
        first = s;
    } else {
        return EDEN_DSMOD_FALSE;
    }
    NewPageIfChanged(in);
    return EDEN_DSMOD_TRUE;
}

constexpr ffr::Rgba kGold = ffr::C(0xFFE6C36B), kBlue = ffr::C(0xFF5AA8F0);  // the objectives: main story, side quests (the game's own blue)

// The map's pictures: the area's capture in the game's map tone, an island padded into its marker square, a UI texture as is,
// the objectives and their arrows, and the zoom buttons.
bool MapPicture(Instance* in, const std::string& k, void* receiver, EdenDsmodImageSink sink) {
    auto send = [&](const ffr::Canvas& c) { sink(receiver, uint32_t(c.w()), uint32_t(c.h()), c.pixels().data(), c.pixels().size()); return true; };
    auto img = [&](const std::string& path) { return in->assets.Image(path); };
    if (k.rfind("ui:", 0) == 0) {
        const ffr::Image* im = img("FFRS/Content/UI/Textures/" + k.substr(3) + ".uasset");
        if (!im) return false;
        sink(receiver, uint32_t(im->w), uint32_t(im->h), im->rgba.data(), im->rgba.size());
        return true;
    }
    if (k.rfind("cap:", 0) == 0) {
        const ffr::Image* im = img(k.substr(4));
        if (!im) return false;
        // The capture in the middle square, transparent around it: the menu art ("bg:map") sits fixed behind the widget, so only
        // the map moves when it follows the player
        const int n = 1024, pad = int(std::lround(double(n) * kTownPad / kTownN)), w = n + 2 * pad;
        ffr::Canvas c(w, n);
        c.Blit(*im, pad, 0, n, n, 0.95f, ffr::C(0xFFC4D4DE));
        return send(c);
    }
    if (k.rfind("bg:", 0) == 0) {  // bg:<w>x<h>: the game's menu art (cover, aspect kept) at a widget's size, darkened so the map reads first
        int w = 0, h = 0;
        if (std::sscanf(k.c_str() + 3, "%dx%d", &w, &h) != 2 || w < 16 || h < 16 || w > 2048 || h > 2048) return false;
        ffr::Canvas c(w, h);
        c.Clear(ffr::C(0xFF0A131B));
        if (const ffr::Image* bg = img("FFRS/Content/UI/MainMenu/Common/BG/Textures/T_MenuBG001.uasset")) c.BlitFit(*bg, 0, 0, w, h, 1.f, true);
        c.Fill(0, 0, w, h, ffr::C(0xB0000000));
        return send(c);
    }
    if (k.rfind("isl:", 0) == 0) {  // isl:<picture size>:<fraction of the marker square>:<texture>
        const size_t c1 = k.find(':', 4), colon = c1 == std::string::npos ? c1 : k.find(':', c1 + 1);
        if (colon == std::string::npos) return false;
        const int n = std::clamp(std::atoi(k.substr(4, c1 - 4).c_str()), 64, 1024);
        const double frac = std::clamp(std::atof(k.substr(c1 + 1, colon - c1 - 1).c_str()), 0.05, 1.0);
        const ffr::Image* im = img(k.substr(colon + 1));
        if (!im) return false;
        const int d = int(std::lround(n * frac));
        ffr::Canvas c(n, n);
        c.Blit(*im, (n - d) / 2, (n - d) / 2, d, d);
        return send(c);
    }
    if (k.rfind("btn:", 0) == 0) {  // a round map button: zoom in, zoom out
        ffr::Canvas c(96, 96);
        c.Disc(48, 48, 44, ffr::C(0xFF5EC4E6));
        c.Disc(48, 48, 41, ffr::C(0xF0101E28));
        const ffr::Rgba ink = ffr::C(0xFFF2F5F7);
        const std::string b = k.substr(4);
        if (b == "in" || b == "out") c.Fill(28, 45, 40, 6, ink);
        if (b == "in") c.Fill(45, 28, 6, 40, ink);
        if (b == "home") {
            c.Disc(48, 48, 20, ink);
            c.Disc(48, 48, 15, ffr::C(0xF0101E28));
            c.Disc(48, 48, 7, ffr::C(0xFFE6C36B));
            c.Fill(46, 18, 4, 12, ink), c.Fill(46, 66, 4, 12, ink), c.Fill(18, 46, 12, 4, ink), c.Fill(66, 46, 12, 4, ink);
        }
        return send(c);
    }
    // a small blue badge on a gold one's lower right: a place both the story and a side quest lead to
    auto side_badge = [&](ffr::Canvas& c, float cx, float cy, float r) {
        c.Disc(cx, cy, r + 4, ffr::C(0x90000000));
        c.Disc(cx, cy, r, kBlue);
        c.Disc(cx, cy, r * 0.74f, ffr::C(0xF0182C3A));
        c.Fill(int(cx - r * 0.12f), int(cy - r * 0.5f), int(r * 0.24f) + 1, int(r * 0.55f), ffr::C(0xFFF2F5F7));
        c.Fill(int(cx - r * 0.12f), int(cy + r * 0.18f), int(r * 0.24f) + 1, int(r * 0.24f) + 1, ffr::C(0xFFF2F5F7));
    };
    if (k == "goal" || k == "goal:side" || k == "goal:both") {  // an objective: the game's quest map icon on a gold (side quests: blue) badge
        ffr::Canvas c(128, 128);
        const bool both = k == "goal:both";
        const float r = both ? 46.f : 54.f, cx = both ? 56.f : 64.f;
        c.Disc(cx, cx, r + 6, ffr::C(0x60000000));
        c.Disc(cx, cx, r, k == "goal:side" ? kBlue : kGold);
        c.Disc(cx, cx, r - 7, ffr::C(0xF0182C3A));
        const int is = int(r * 1.48f);
        if (const ffr::Image* ic = img("FFRS/Content/UI/Textures/Icon/MAP/ICON_MAP_L_Check.uasset")) c.Blit(*ic, int(cx) - is / 2, int(cx) - is / 2, is, is);
        else c.Fill(int(cx) - 6, int(cx) - 30, 12, 40, ffr::C(0xFFF2F5F7)), c.Fill(int(cx) - 6, int(cx) + 18, 12, 12, ffr::C(0xFFF2F5F7));
        if (both) side_badge(c, 100, 100, 22);
        return send(c);
    }
    if (k.rfind("arrow:", 0) == 0 && k.size() >= 9) {  // arrow:<m|s|b (main, side, both)>:<15 degree step, 0 = right, clockwise>
        const ffr::Rgba col = k[6] == 's' ? kBlue : kGold;
        const double rad = std::atoi(k.c_str() + 8) * 15 * M_PI / 180, ux = std::cos(rad), uy = std::sin(rad);
        ffr::Canvas c(128, 128);
        auto tri = [&](double tip, double back, double half, ffr::Rgba cc) {  // a point toward the objective
            c.Triangle(float(64 + ux * tip), float(64 + uy * tip), float(64 + ux * back - uy * half), float(64 + uy * back + ux * half),
                       float(64 + ux * back + uy * half), float(64 + uy * back - ux * half), cc);
        };
        tri(63, 20, 30, ffr::C(0x70000000));
        tri(58, 24, 24, col);
        c.Disc(64, 64, 34, ffr::C(0x70000000));
        c.Disc(64, 64, 31, col);
        c.Disc(64, 64, 26, ffr::C(0xF0182C3A));
        if (const ffr::Image* ic = img("FFRS/Content/UI/Textures/Icon/MAP/ICON_MAP_L_Check.uasset")) c.Blit(*ic, 38, 38, 52, 52);
        else c.Fill(60, 46, 8, 22, ffr::C(0xFFF2F5F7)), c.Fill(60, 72, 8, 8, ffr::C(0xFFF2F5F7));
        if (k[6] == 'b') side_badge(c, 64 - float(ux) * 30 + float(uy) * 22, 64 - float(uy) * 30 - float(ux) * 22, 15);  // behind, off to one side
        return send(c);
    }
    // The world map's places, the scope switch, Back and the place card: the game's map palette (deep navy, its light blue
    // frame, gold for the player and what can be done)
    const ffr::Rgba kInk = ffr::C(0xFFF2F5F7), kNavy = ffr::C(0xFF101E28), kFrameBlue = ffr::C(0xFF5EC4E6), kDim = ffr::C(0xFF8A99A6);
    auto pill = [](ffr::Canvas& c, float x, float y, float w, float h, ffr::Rgba col) {
        const float r = h / 2;
        c.Disc(x + r, y + r, r, col);
        c.Disc(x + w - r, y + r, r, col);
        c.Fill(int(x + r), int(y), int(w - 2 * r), int(h), col);
    };
    auto field = [&](const std::string& key, int n) {  // the n-th ':' separated field of key; the last takes the rest
        size_t a = 0;
        for (int i = 0; i < n && a != std::string::npos; ++i) a = key.find(':', a), a = a == std::string::npos ? a : a + 1;
        if (a == std::string::npos) return std::string();
        return key.substr(a);
    };
    auto upto = [](const std::string& v) { const size_t c = v.find(':'); return c == std::string::npos ? v : v.substr(0, c); };
    // the game's map icon for a kind of place: c a dungeon (its cave), s a shrine, t a town
    auto place_icon = [&](char ic) {
        return img(std::string("FFRS/Content/UI/Textures/Icon/MAP/ICON_MAP_L_") + (ic == 'c' ? "Cave" : ic == 's' ? "Shrine" : "Town") + ".uasset");
    };
    if (k.rfind("lm:", 0) == 0 && k.size() >= 5) {  // lm:<h here | t can travel | d cannot><1 selected><icon>
        const char st = k[3];
        const bool sel = k[4] == '1';
        const char icon = k.size() > 5 ? k[5] : 't';
        ffr::Canvas c(112, 112);
        const ffr::Rgba ring = st == 'h' ? kGold : st == 't' ? kFrameBlue : kDim;
        if (sel) c.Disc(56, 56, 54, ffr::C(0xC0F2F5F7));
        c.Disc(56, 56, 46, ffr::C(0x90000000));
        c.Disc(56, 56, 42, ring);
        c.Disc(56, 56, 36, st == 'd' ? ffr::C(0xFF1A2630) : kNavy);
        if (const ffr::Image* ic = place_icon(icon)) c.Blit(*ic, 22, 22, 68, 68, st == 'd' ? 0.45f : 1.f);
        else c.Disc(56, 56, 14, ring);
        return send(c);
    }
    if (k.rfind("scope:", 0) == 0) {  // scope:<1 world shown | 0 the place>:<place name>: World > Village of Kol
        const bool world = field(k, 1).rfind("1", 0) == 0;
        const std::string place = field(k, 2);
        const int w = 560, h = 72, split = 196;
        ffr::Canvas c(w, h);
        pill(c, 0, 0, w, h, kFrameBlue);
        pill(c, 2, 2, w - 4, h - 4, kNavy);
        if (world) pill(c, 6, 6, split - 6, h - 12, kGold);
        else pill(c, split + 22, 6, w - split - 28, h - 12, kGold);
        const ffr::Font& f = in->assets.bold;
        c.Text(f, "World", split / 2 + 3, 47, 30, world ? kNavy : kInk, ffr::Align::Center);
        c.Triangle(split + 4, 26, split + 4, 46, split + 15, 36, kDim);  // the breadcrumb's chevron: the place is in the world
        c.Text(f, place, (split + 22 + w - 6) / 2, 47, 30, world ? kInk : kNavy, ffr::Align::Center, float(w - split - 60));
        return send(c);
    }
    if (k.rfind("back:", 0) == 0) {  // back:<place name>: the way back to the player
        const int w = 420, h = 76;
        ffr::Canvas c(w, h);
        pill(c, 0, 0, w, h, ffr::C(0xFF8A6A1E));
        pill(c, 3, 3, w - 6, h - 6, kGold);
        c.Triangle(30, 38, 48, 24, 48, 52, kNavy);  // an arrow pointing back
        c.Fill(46, 33, 18, 10, kNavy);
        c.Text(in->assets.bold, "Back to " + field(k, 1), 80, 49, 30, kNavy, ffr::Align::Left, float(w - 104));
        return send(c);
    }
    if (k.rfind("card:", 0) == 0) {  // card:<state>:<kind>:<name>: a place tapped on the world map, and its fast travel
        const char st = k.size() > 5 ? k[5] : 'T';
        const std::string kind = upto(field(k, 2)), name = field(k, 3).empty() ? kind : field(k, 3);
        const int w = 620, h = 124;
        ffr::Canvas c(w, h);
        c.Fill(0, 0, w, h, kFrameBlue);
        c.Fill(2, 2, w - 4, h - 4, ffr::C(0xFF0E1A23));
        c.GradientV(2, 2, w - 4, h / 2, ffr::C(0xFF1B2C38), ffr::C(0xFF0E1A23));
        const bool here = st == 'H', can = st == 'T' || st == 'O' || st == 'R' || st == 'C' || st == 'F' || st == 'B' || st == 'D';
        const char why = st == 'R' && k.size() > 6 ? k[6] : '?';
        const ffr::Rgba ring = here ? kGold : can ? kFrameBlue : kDim;
        c.Disc(62, 62, 40, ring);
        c.Disc(62, 62, 34, kNavy);
        if (const ffr::Image* ic = place_icon(kind == "Dungeon" ? 'c' : kind == "Shrine" ? 's' : 't')) c.Blit(*ic, 30, 30, 64, 64, can || here ? 1.f : 0.5f);
        const ffr::Font& title = in->assets.title.ok() ? in->assets.title : in->assets.bold;
        const int bx = w - 216, bw = 200;  // the button (or the status) on the right
        const bool button = st == 'T' || st == 'F' || st == 'O';
        const float text_w = float(button ? bx - 130 : w - 118 - 24);  // the text runs to the edge when no button is there
        c.Text(title, name, 118, 58, 36, kInk, ffr::Align::Left, text_w);
        std::string sub = kind, status;
        switch (st) {
        case 'H': sub += "  \xC2\xB7  You are here"; break;
        case 'V': sub += "  \xC2\xB7  Not visited yet"; break;
        case 'O': sub = "Travelling\xE2\x80\xA6"; break;
        case 'R':
            sub = why == 'E' ? "Not now: a story event is under way" : why == 'L' ? "That place is closed for now"
                : why == 'M' ? "No fast travel from here" : "The game won't travel there now";
            break;
        case 'C': sub = "Closing the game's map\xE2\x80\xA6"; break;
        case 'F': sub = "The game's map did not open"; break;
        case 'B': sub = "Close the game's menu to fast travel"; break;
        case 'D': sub = "Leave the dungeon to fast travel"; break;
        default: sub += "  \xC2\xB7  Fast travel"; break;
        }
        c.Text(in->assets.regular, sub, 118, 96, 25, st == 'R' || st == 'F' || st == 'B' || st == 'D' ? kGold : kDim, ffr::Align::Left, text_w);
        if (st == 'T' || st == 'F') {  // Fast Travel (again after a failure)
            pill(c, bx, 26, bw, 72, ffr::C(0xFF8A6A1E));
            pill(c, bx + 3, 29, bw - 6, 66, kGold);
            c.Text(in->assets.bold, "Fast Travel", bx + bw / 2, 72, 29, kNavy, ffr::Align::Center);
        } else if (st == 'O') {  // Cancel while it runs
            pill(c, bx, 26, bw, 72, kFrameBlue);
            pill(c, bx + 3, 29, bw - 6, 66, kNavy);
            c.Text(in->assets.bold, "Cancel", bx + bw / 2, 72, 29, kInk, ffr::Align::Center);
        }
        return send(c);
    }
    if (k == "blank") return send(ffr::Canvas(4, 4));  // transparent
    if (k.rfind("ocean:", 0) == 0 && k.size() >= 8) {  // ocean:<flip x><flip y>: mirrored tiles meet without seams
        const ffr::Image* im = img("FFRS/Content/UI/Textures/MapCapture/Wld/T_Wld_ocean.uasset");
        if (!im) return false;
        const bool fx = k[6] == '1', fy = k[7] == '1';
        const int n = 512;  // (a soft texture: a quarter of the memory, nothing lost on screen)
        ffr::Canvas c(n, n);
        c.Blit(*im, 0, 0, n, n);
        if (fx || fy) {
            std::vector<uint8_t> px = c.pixels(), out(px.size());
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    std::memcpy(&out[(size_t(y) * n + x) * 4], &px[(size_t(fy ? n - 1 - y : y) * n + (fx ? n - 1 - x : x)) * 4], 4);
            c.CopyFrom(out);
        }
        return send(c);
    }
    return false;
}

EdenDsmodBool LoadImage(void* p, const EdenDsmodHostApi* host, const char* key, void* receiver, EdenDsmodImageSink sink) {
    auto* in = static_cast<Instance*>(p);
    if (!key || std::strncmp(key, kKeys, std::strlen(kKeys)) != 0) return EDEN_DSMOD_FALSE;
    if (std::strncmp(key, kPrefix, std::strlen(kPrefix)) != 0) {
        try {
            std::lock_guard<std::mutex> alk(in->assets.mu);
            if (!in->assets.Ensure(host)) return EDEN_DSMOD_FALSE;
            return MapPicture(in, key + std::strlen(kKeys), receiver, sink) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
        } catch (...) {
            return EDEN_DSMOD_FALSE;
        }
    }
    try {
        std::lock_guard<std::mutex> alk(in->assets.mu);
        const bool art = in->assets.Ensure(host);
        ffr::GameState s;
        ffr::View v;
        {
            std::lock_guard<std::mutex> lk(in->state_mu);
            s = in->state;
            v = in->view;
        }
        for (auto& q : s.quests)  // names the game has not looked up yet: through its own localization files
            if (q.name.empty() && !q.name_src.empty()) {
                const auto k = in->assets.quest_keys.find(q.name_ns + '\x1f' + q.name_src);
                if (k == in->assets.quest_keys.end()) continue;
                const auto t = in->assets.loc.find(q.name_ns + '\x1f' + k->second);
                if (t != in->assets.loc.end()) q.name = t->second;
            }
        ffr::Canvas c(ffr::kW, ffr::kH);
        if (!art) {
            static ffr::Font none;
            c.Clear(ffr::C(0xFF0A131B));
            Log(host, EDEN_DSMOD_LOG_WARNING, "game files: " + in->assets.why);
            sink(receiver, uint32_t(c.w()), uint32_t(c.h()), c.pixels().data(), c.pixels().size());
            return EDEN_DSMOD_TRUE;
        }
        ffr::Art a;
        a.image = [&](const std::string& path) { return in->assets.Image(path); };
        a.text = [&](const std::string& ns, const std::string& k) {
            auto it = in->assets.loc.find(ns + '\x1f' + k);
            return it == in->assets.loc.end() ? std::string() : it->second;
        };
        a.regular = &in->assets.regular;
        a.bold = &in->assets.bold;
        a.title = &in->assets.title;
        ffr::ComposePage(c, s, v, a);
        sink(receiver, uint32_t(c.w()), uint32_t(c.h()), c.pixels().data(), c.pixels().size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

void Configure(void* p, const EdenDsmodHostExtensions* ext) {
    auto* in = static_cast<Instance*>(p);
    std::lock_guard<std::mutex> lk(in->state_mu);
    in->helper_beat = 0, in->helper_beat_ms = 0, in->helper_logged = false;
    const bool bound = in->helper.Bind(ext);
    Log(&in->host, EDEN_DSMOD_LOG_INFO, bound ? "helper: mailbox bound" : "helper: no mailbox (fast travel through the game's map)");
}

const EdenDsmodModuleApi kApi = {
    EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0, EDEN_DSMOD_MODULE_ABI_HASH, kTitle,
    "FINAL FANTASY RESONANCE dual screen", EDEN_DSMOD_CAP_EXTENSIONS, SupportsBuild, Create, Destroy, Sample, Tick,
};
const EdenDsmodModuleExtensions kExt = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH, Configure, OnAction, LoadImage,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t v, uint64_t h) {
    return v == EDEN_DSMOD_MODULE_ABI_VERSION && h == EDEN_DSMOD_MODULE_ABI_HASH ? &kApi : nullptr;
}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t v, uint64_t h) {
    return v == EDEN_DSMOD_EXT_VERSION && h == EDEN_DSMOD_EXT_HASH ? &kExt : nullptr;
}
