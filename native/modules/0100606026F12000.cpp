// FINAL FANTASY RESONANCE (0100606026F12000) companion module, update 1.0.1.
// Party, equipment, items, the live map (towns, dungeons and the world map, with pinch zoom) and the battle, drawn in the
// game's own look from the player's game files (fonts, icons, portraits, map captures, English text). Nothing from the game
// is shipped. It never writes the game's memory: in a battle, a tap on the command window presses the same buttons the
// player would (the package's enforce rules), step by step, checking the game's own cursor after every press.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include "ffr_assets.h"
#include "ffr_draw.h"
#include "ffr_pages.h"
#include "ffr_reader.h"

#ifdef FFR_DEV
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
constexpr int kIslandClasses = 6, kIslandsPerClass = 12, kMaxIcons = 64;
struct MapFrame {
    int mode = 0;  // 0 none, 1 an area's capture, 2 the world
    double ox = 0, oy = 0, k = 1, n = kTownN;  // map = (land - origin) * k, y flipped
    std::string base;                         // mode 1: the capture picture's key
    double v[4] = {};                         // the default view: x0 y0 x1 y1
    struct Pin { double x, y; std::string src; int cls = 0; };
    std::vector<Pin> islands, icons, goal;
    double X(double lx) const { return (lx - ox) * k; }
    double Y(double ly) const { return n - (ly - oy) * k; }
};

MapFrame Frame(const ffr::MapState& m) {
    MapFrame f;
    if (!m.valid) return f;
    if (m.world) {
        // Every island's capture covers a square of side max(bw, bh) around its location. They all draw as markers of one world
        // size (kIslandN), each picture scaled inside its own square (the module pads it: "isl:" keys), so L land units per marker.
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18, smax = 0;
        for (auto& is : m.islands) {
            const double sd = std::max(is.bw, is.bh);
            x0 = std::min(x0, is.wx - sd / 2), x1 = std::max(x1, is.wx + sd / 2);
            y0 = std::min(y0, is.wy - sd / 2), y1 = std::max(y1, is.wy + sd / 2);
            smax = std::max(smax, sd);
        }
        if (smax <= 0) return f;
        const double span = std::max(x1 - x0, y1 - y0);
        f.mode = 2, f.n = kWorldN;
        f.k = std::min(kWorldN * 0.92 / span, kIslandSizes[0] / smax);
        f.ox = (x0 + x1) / 2 - kWorldN / 2 / f.k, f.oy = (y0 + y1) / 2 - kWorldN / 2 / f.k;
        int per[kIslandClasses] = {};
        for (auto& is : m.islands) {
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
        // The default view follows the player (Sample), about as close as the game's own map opens
        (void)span;
    } else if (!m.texture.empty() && m.bw > 0 && m.bh > 0) {
        const double side = std::max(m.bw, m.bh);
        f.mode = 1, f.n = kTownN, f.k = kTownN / side;
        f.ox = m.wx - side / 2, f.oy = m.wy - side / 2;
        f.base = std::string(kKeys) + "cap:" + m.texture;
        f.v[0] = f.X(m.wx - m.bw / 2), f.v[2] = f.X(m.wx + m.bw / 2), f.v[1] = f.Y(m.wy + m.bh / 2), f.v[3] = f.Y(m.wy - m.bh / 2);
    } else {
        return f;
    }
    if (m.has_goal) {  // the objective, when it is in this map
        const double x = f.X(m.goal_x), y = f.Y(m.goal_y);
        if (x >= 0 && y >= 0 && x <= f.n && y <= f.n) f.goal.push_back({x, y, std::string(kKeys) + "goal"});
    }
    for (auto& ic : m.icons) {  // (the reader leaves the world map's empty)
        const double x = f.X(ic.x), y = f.Y(ic.y);
        if (x < 0 || y < 0 || x > f.n || y > f.n || int(f.icons.size()) >= kMaxIcons) continue;
        f.icons.push_back({x, y, std::string(kKeys) + "ui:Icon/MAP/" + ic.icon});
    }
    return f;
}

// Taps on the command window become the presses a player would make. One press at a time; the next only once the game's
// cursor has had time to move, and A only when the cursor (or the target) is the one tapped.
enum Btn { kBtnNone = -1, kBtnA, kBtnB, kBtnUp, kBtnDown, kBtnLeft, kBtnRight, kBtnCount };
const char* kBtnPoints[kBtnCount] = {"ffr.drv.a", "ffr.drv.b", "ffr.drv.up", "ffr.drv.down", "ffr.drv.left", "ffr.drv.right"};
struct Drive {
    int kind = 0;       // 0 idle, 1 choose an entry, 2 choose a target, 3 back, 4 a resonance (want: its EUINavigation direction)
    int want = -1;      // the entry index
    uint64_t unit = 0;  // kind 2: the CPP_BtlUnit to target
    ffr::ActionRef key;  // the entry tapped (it must still be there)
    uint64_t start = 0, next = 0;
    int presses = 0, dir = 0, dir_presses = 0;
};

struct Assets {
    std::mutex mu;
    bool tried = false, ok = false;
    std::string why;
    ffr::IoStore io;
    ffr::Pak pak;
    std::unordered_map<std::string, std::string> loc;
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
    std::vector<std::array<std::string, 3>> icon_keys, goal_keys, island_keys[kIslandClasses];
    uint64_t serial = 0, last_key = 0;
    uint32_t ticks = 0;
    std::string page_key;
#ifdef FFR_DEV
    std::string dev_last;
    std::atomic<bool> dev_stop{false};
    int dev_fd = -1;
    std::thread dev_thread;
#endif
};

void Log(const EdenDsmodHostApi* h, uint32_t lvl, const std::string& s) {
    if (h && h->log) h->log(h->userdata, lvl, ("ffr: " + s).c_str());
}

#ifdef FFR_DEV
// Development memory server (same protocol as tools/ffr/probe): 127.0.0.1:47123 through adb forward.
bool SendAll(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) { ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w <= 0) return false; c += w; n -= size_t(w); }
    return true;
}
bool RecvAll(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n) { ssize_t r = recv(fd, c, n, 0); if (r <= 0) return false; c += r; n -= size_t(r); }
    return true;
}
void DevServe(Instance* in) {
    while (!in->dev_stop) {
        int fd = accept(in->dev_fd, nullptr, nullptr);
        if (fd < 0) { usleep(100000); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        std::vector<uint8_t> buf;
        for (;;) {
            uint8_t hdr[13];
            if (!RecvAll(fd, hdr, 13)) break;
            uint64_t a; uint32_t len;
            std::memcpy(&a, hdr + 1, 8); std::memcpy(&len, hdr + 9, 4);
            if (hdr[0] == 1) {
                if (len > (16u << 20)) len = 0;
                buf.resize(len);
                uint8_t ok = len && in->host.read_memory(in->host.userdata, a, buf.data(), len) ? 1 : 0;
                if (!SendAll(fd, &ok, 1) || (ok && !SendAll(fd, buf.data(), len))) break;
            } else if (hdr[0] == 2) {
                uint64_t v[5] = {in->host.main_base, in->host.main_size, in->host.get_heap_begin(in->host.userdata), in->host.get_heap_end(in->host.userdata), in->ticks};
                if (!SendAll(fd, v, sizeof v)) break;
            } else break;
        }
        close(fd);
    }
}
#endif

EdenDsmodBool SupportsBuild(const char* b) {
    return b && std::strncmp(b, "84E182F72EE1DA4F", 16) == 0 ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    auto* in = new Instance();
    in->host = *host;
    char hex[17] = {};
    for (int i = 0; i < 8; ++i) std::snprintf(hex + 2 * i, 3, "%02X", host->build_id[i]);
    in->reader_ok = in->reader.Init(host, hex, in->reader_why);
    Log(host, in->reader_ok ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_WARNING, in->reader_ok ? "reader ready" : "reader: " + in->reader_why);
#ifdef FFR_DEV
    in->dev_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(in->dev_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET; sa.sin_port = htons(47123); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(in->dev_fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && listen(in->dev_fd, 1) == 0) in->dev_thread = std::thread(DevServe, in);
#endif
    return in;
}

void Destroy(void* p) {
    auto* in = static_cast<Instance*>(p);
#ifdef FFR_DEV
    in->dev_stop = true;
    if (in->dev_fd >= 0) { shutdown(in->dev_fd, SHUT_RDWR); close(in->dev_fd); }
    if (in->dev_thread.joinable()) in->dev_thread.join();
#endif
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

uint64_t ViewKey(const ffr::View& v) {
    return uint64_t(v.map_ok) << 60 | uint64_t(v.tab) | uint64_t(v.member + 1) << 4 | uint64_t(v.item_cat) << 12 | uint64_t(v.item_scroll) << 16 | uint64_t(v.item_sel + 1) << 32;
}

void NewPageIfChanged(Instance* in) {  // state_mu held
    const uint64_t key = in->state.Hash() * 31 + ViewKey(in->view);
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
        in->state = std::move(s);
        NewPageIfChanged(in);
    } else if (in->ticks % 2 == 1 && in->reader_ok) {  // in a battle, HP, MP and the command cursor in between: ~30 times a second
        std::lock_guard<std::mutex> lk(in->state_mu);
        if (in->state.in_battle && in->reader.RefreshBattle(host, in->state) && in->reader.RefreshMenu(host, in->state)) NewPageIfChanged(in);
    }
    std::lock_guard<std::mutex> lk(in->state_mu);
#ifdef FFR_DEV
    {  // the battle menu as read, whenever it changes (adb logcat): the offsets are still being checked
        const ffr::BattleMenu& mn = in->state.menu;
        std::string sum = "menu battle=" + std::to_string(in->state.in_battle) + " valid=" + std::to_string(mn.valid) + " open=" + std::to_string(mn.open) +
                          " state=" + std::to_string(mn.state) + " cursor=" + std::to_string(mn.cursor) + " vm=" + std::to_string(mn.vm != 0) +
                          " list=" + std::to_string(mn.list != 0) + " actor=" + std::to_string(mn.actor) + " entries=";
        for (auto& e : mn.entries) sum += std::to_string(e.cat) + ":" + std::to_string(e.id) + ",";
        sum += " path=";
        for (auto& e : mn.path) sum += std::to_string(e.cat) + ":" + std::to_string(e.id) + ",";
        sum += " targets=";
        auto units = [&](const std::vector<uint64_t>& v) {
            for (uint64_t t : v) {
                int idx = -1;
                for (size_t i = 0; i < in->state.battle.size(); ++i) if (in->state.battle[i].unit == t) idx = int(i);
                sum += std::to_string(idx) + ",";
            }
        };
        units(mn.targets);
        sum += " sub=";
        units(mn.sub);
        sum += " fin=";
        for (auto& f : mn.finish) sum += std::to_string(f.dir) + ":" + std::to_string(f.skill) + ":" + std::to_string(f.uid) + (f.enable ? "" : "x") + (f.unit ? "u" : "") + ",";
        sum += " bp=";
        for (auto& b : in->state.battle) {
            int oi = -1;
            for (size_t j = 0; j < in->state.battle.size(); ++j) if (b.owner && in->state.battle[j].unit == b.owner) oi = int(j);
            sum += std::to_string(b.bp) + "/" + std::to_string(b.max_bp) + (b.staggered ? "S" : "") + (oi >= 0 ? "v" + std::to_string(oi) : "") + ",";
        }
        sum += " focus=" + std::to_string(mn.focus.cat) + ":" + std::to_string(mn.focus.id) + " press=" + std::to_string(in->press) + " drive=" + std::to_string(in->drive.kind);
        if (in->state.in_battle && sum != in->dev_last) Log(host, EDEN_DSMOD_LOG_INFO, sum + " " + in->reader.Debug());
        in->dev_last = sum;
    }
#endif
    StepDrive(in);
    for (int b = 0; b < kBtnCount; ++b) host->publish_i64(host->userdata, kBtnPoints[b], in->press == b);
    host->publish_i64(host->userdata, "ffr.drv.pending", in->press != kBtnNone);
    host->publish_i64(host->userdata, "ffr.battle_menu", in->view.tab == ffr::kTabBattle && in->state.in_battle);
    // The live map: the frame's statics and the player, every tick (the host keeps nothing between samples)
    const MapFrame& f = in->frame;
    const bool on_map = in->view.tab == ffr::kTabMap && in->view.map_ok && in->state.in_game && in->state.save_ok && f.mode;
    host->publish_i64(host->userdata, "ffr.map.town", on_map && f.mode == 1);
    host->publish_i64(host->userdata, "ffr.map.world", on_map && f.mode == 2);
    if (on_map) {
        // The player 30 times a second; the last position is published on every tick, as the host keeps nothing between samples
        if (in->ticks % 2 == 0 && in->reader_ok && in->reader.ReadPlayer(host, in->live)) in->have_live = true;
        if (in->have_live) {
            host->publish_f64(host->userdata, "ffr.map.px", f.X(in->live.px));
            host->publish_f64(host->userdata, "ffr.map.py", f.Y(in->live.py));
            host->publish_f64(host->userdata, "ffr.map.yaw", in->live.yaw);
        }
        if (f.mode == 1) host->publish_text(host->userdata, "ffr.map.base", f.base.c_str());
        double v[4] = {f.v[0], f.v[1], f.v[2], f.v[3]};
        if (f.mode == 2 && in->have_live) {  // the world: a window around the player, as close as the game's map opens; it follows
            const double hw = 13000 * f.k, hh = hw * ffr::kMapH / ffr::kMapW;  // until the player pans or zooms
            const double cx = f.X(in->live.px), cy = f.Y(in->live.py);
            v[0] = cx - hw, v[2] = cx + hw, v[1] = cy - hh, v[3] = cy + hh;
        }
        static const char* kView[4] = {"ffr.map.vx0", "ffr.map.vy0", "ffr.map.vx1", "ffr.map.vy1"};
        if (f.mode == 1 || in->have_live)
            for (int i = 0; i < 4; ++i) host->publish_f64(host->userdata, kView[i], v[i]);
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
        pins(f.icons, in->icon_keys, "icon", kMaxIcons);
        pins(f.goal, in->goal_keys, "goal", 1);
        for (int c = 0; c < kIslandClasses; ++c) {
            std::vector<MapFrame::Pin> v;
            for (auto& p : f.islands)
                if (p.cls == c) v.push_back(p);
            pins(v, in->island_keys[c], ("isl" + std::to_string(c) + "_").c_str(), kIslandsPerClass);
        }
    }
    host->publish_text(host->userdata, "ffr.page", in->page_key.c_str());
    const int tab = in->view.tab;
    host->publish_i64(host->userdata, "ffr.tab", tab);
    host->publish_i64(host->userdata, "ffr.in_game", in->state.in_game && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.map_tab", in->state.in_game && in->state.save_ok && in->view.map_ok);
    host->publish_i64(host->userdata, "ffr.party_list", tab == ffr::kTabParty && in->view.member < 0 && in->state.save_ok);
    host->publish_i64(host->userdata, "ffr.party_detail", tab == ffr::kTabParty && in->view.member >= 0);
    host->publish_i64(host->userdata, "ffr.items", tab == ffr::kTabItems && in->state.save_ok);
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
    } else if (a == "member") {
        if (arg < 0 || arg >= int(ffr::Roster(in->state).size())) return EDEN_DSMOD_FALSE;
        v.member = int(arg);
    } else if (a == "back") {
        v.member = -1;
    } else if (a == "cat") {
        if (arg < 0 || arg > 3) return EDEN_DSMOD_FALSE;
        v.item_cat = int(arg);
        v.item_scroll = 0;
        v.item_sel = -1;
    } else if (a == "row") {
        const int idx = v.item_scroll + int(arg);
        if (arg < 0 || idx >= int(ffr::ItemsOf(in->state, v.item_cat).size())) return EDEN_DSMOD_FALSE;
        v.item_sel = v.item_sel == idx ? -1 : idx;
    } else if (a == "scroll") {
        const int n = int(ffr::ItemsOf(in->state, v.item_cat).size());
        const int s = std::max(0, std::min(v.item_scroll + int(arg) * (ffr::kItemRows - 1), n - ffr::kItemRows));
        if (s == v.item_scroll) return EDEN_DSMOD_FALSE;
        v.item_scroll = s;
    } else {
        return EDEN_DSMOD_FALSE;
    }
    NewPageIfChanged(in);
    return EDEN_DSMOD_TRUE;
}

// The map's pictures: the area's capture in the game's map tone, an island padded into its marker square, a UI texture as is,
// and the zoom buttons.
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
        // Over the game's menu art (cover, aspect kept), darkened so the map reads first; the capture in the middle square
        const int n = 1024, pad = int(std::lround(double(n) * kTownPad / kTownN)), w = n + 2 * pad;
        ffr::Canvas c(w, n);
        c.Clear(ffr::C(0xFF0A131B));
        if (const ffr::Image* bg = img("FFRS/Content/UI/MainMenu/Common/BG/Textures/T_MenuBG001.uasset")) c.BlitFit(*bg, 0, 0, w, n, 1.f, true);
        c.Fill(0, 0, w, n, ffr::C(0xB0000000));
        c.Blit(*im, pad, 0, n, n, 0.95f, ffr::C(0xFFC4D4DE));
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
    if (k.rfind("btn:", 0) == 0) {  // a round map button: zoom in, zoom out, back to the player
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
    if (k == "goal") {  // the objective: the game's "!" map icon on a gold badge, so it stands out from the other icons
        ffr::Canvas c(128, 128);
        c.Disc(64, 64, 60, ffr::C(0x60000000));
        c.Disc(64, 64, 54, ffr::C(0xFFE6C36B));
        c.Disc(64, 64, 47, ffr::C(0xF0182C3A));
        if (const ffr::Image* ic = img("FFRS/Content/UI/Textures/Icon/MAP/ICON_MAP_L_Check.uasset")) c.Blit(*ic, 24, 24, 80, 80);
        else c.Fill(58, 34, 12, 40, ffr::C(0xFFF2F5F7)), c.Fill(58, 82, 12, 12, ffr::C(0xFFF2F5F7));
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

void Configure(void*, const EdenDsmodHostExtensions*) {}

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
