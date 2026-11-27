// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Real module ABI preview using separately supplied private memdump sidecars and merged RomFS.
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "xcx_battle.h"
#include "xcx_helper_mailbox.h"
#include "xcx_maptiles.h"
#include "xcx_reader.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <vector>
using Json = nlohmann::json;
namespace fs = std::filesystem;
void Check(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
struct Block {
    std::uint64_t address;
    std::vector<std::uint8_t> bytes;
};
struct Host {
    fs::path romfs, out;
    std::vector<Block> blocks;
    std::string frame;
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::int64_t> flags; // runtime flags the module may read (@flag:<name>)
    std::map<std::string, std::int64_t> view_i64; // other runtime keys the test sets (@view_touch_down:<id>)
    std::atomic<int> read_delay_ms{0};          // emulates slow game files (NFS): every RomFS read waits this long
    std::map<std::string, double> floats, view_f64; // published floats; the runtime's @map_view_* the test sets
    std::map<std::string, std::string> texts;       // published texts
    int writes{}, reads{};
    std::atomic<int> rom_reads{};
    bool read_throws{};
    EdenDsmodHostApi api{};
    explicit Host(fs::path rom, fs::path directory, fs::path output) : romfs(rom), out(output) {
        const auto ownership = Json::parse(std::ifstream(directory / "ownership.json"));
        for (const auto &subtree : {"field-baseline", "saved-field"})
            for (const auto &entry : fs::directory_iterator(directory / subtree)) {
                if (entry.path().extension() != ".json" ||
                    !entry.path().string().ends_with(".bin.json"))
                    continue;
                auto m = Json::parse(std::ifstream(entry.path()));
                Check(m["run_id"] == ownership["run_id"], "Mixed fixture run");
                std::ifstream f(entry.path().string().substr(0, entry.path().string().size() - 5),
                                std::ios::binary);
                std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
                Check(bytes.size() == m["length"], "Truncated fixture");
                blocks.push_back(
                    {std::stoull(m["addr"].get<std::string>(), nullptr, 16), std::move(bytes)});
            }
        std::stable_sort(blocks.begin(), blocks.end(), [](const auto &a, const auto &b) {
            return a.bytes.size() > b.bytes.size();
        });
        api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
        api.struct_size = sizeof api;
        api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        api.userdata = this;
        api.title_id = xcx::TitleId;
        api.main_base = 0x80655000;
        api.main_size = 0x2000000;
        for (int i = 0; i < 32; ++i)
            api.build_id[i] = std::stoul(std::string(xcx::Build.substr(i * 2, 2)), nullptr, 16);
        api.is_mapped = [](void *u, std::uint64_t a, std::uint64_t n) -> EdenDsmodBool {
            return static_cast<Host *>(u)->Find(a, n) != nullptr;
        };
        api.read_memory = [](void *u, std::uint64_t a, void *out, std::size_t n) -> EdenDsmodBool {
            auto *h = static_cast<Host *>(u);
            ++h->reads;
            if (h->read_throws)
                throw std::runtime_error("Injected acquisition failure");
            auto *b = h->Find(a, n);
            if (!b)
                return false;
            std::memcpy(out, b->bytes.data() + a - b->address, n);
            return true;
        };
        api.write_memory = [](void *u, std::uint64_t, const void *, std::size_t) -> EdenDsmodBool {
            ++static_cast<Host *>(u)->writes;
            return false;
        };
        api.read_romfs = [](void *u, const char *p, std::uint64_t at, void *out,
                            std::size_t n) -> std::size_t {
            auto *h = static_cast<Host *>(u);
            ++h->rom_reads;
            if (const int d = h->read_delay_ms.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(d));
            fs::path rel(p);
            if (rel.is_absolute())
                rel = rel.relative_path();
            for (const auto &part : rel)
                if (part == "..")
                    return 0;
            std::ifstream f(h->romfs / rel, std::ios::binary);
            if (!f)
                return 0;
            f.seekg(at);
            f.read(static_cast<char *>(out), n);
            return f.gcount();
        };
        api.publish_text = [](void *u, const char *key, const char *value) {
            auto *h = static_cast<Host *>(u);
            if (std::string_view(key) == "xcx.frame")
                h->frame = value;
            h->texts[key] = value;
        };
        api.publish_f64 = [](void *u, const char *key, double value) {
            static_cast<Host *>(u)->floats[key] = value;
        };
        api.get_f64 = [](void *u, const char *name, double fallback) -> double {
            const auto &v = static_cast<Host *>(u)->view_f64;
            const auto it = v.find(name);
            return it == v.end() ? fallback : it->second;
        };
        api.publish_i64 = [](void *u, const char *key, std::int64_t value) {
            static_cast<Host *>(u)->ints[key] = value;
        };
        api.get_i64 = [](void *u, const char *name, std::int64_t fallback) -> std::int64_t {
            const auto &flags = static_cast<Host *>(u)->flags;
            // Like the runtime's host: "@flag:<name>" reads the live settings flag.
            const std::string_view query(name);
            if (!query.starts_with("@flag:")) {
                const auto &v = static_cast<Host *>(u)->view_i64;
                const auto it = v.find(name);
                return it == v.end() ? fallback : it->second;
            }
            const auto it = flags.find(std::string(query.substr(6)));
            return it == flags.end() ? fallback : it->second;
        };
        api.log = [](void *, std::uint32_t, const char *s) { std::cerr << s << '\n'; };
    }
    Block *Find(std::uint64_t a, std::size_t n) {
        if (a > UINT64_MAX - n)
            return nullptr;
        for (auto &b : blocks)
            if (a >= b.address && a - b.address <= b.bytes.size() &&
                n <= b.bytes.size() - (a - b.address))
                return &b;
        return nullptr;
    }
    template <class T> void Put(std::uint64_t a, T value) {
        auto *b = Find(a, sizeof value);
        Check(b, "No mutable fixture range");
        std::memcpy(b->bytes.data() + a - b->address, &value, sizeof value);
    }
};
struct Output {
    fs::path path;
    double marker_x{}, marker_y{};
    std::size_t marker_pixels{};
    std::chrono::steady_clock::time_point requested = std::chrono::steady_clock::now();
    double prepare_ms{};
};
void Sink(void *r, std::uint32_t w, std::uint32_t h, const std::uint8_t *p, std::size_t n) {
    static_cast<Output *>(r)->prepare_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                  static_cast<Output *>(r)->requested)
            .count();
    Check(w == 1240 && h == 1080 && n == std::size_t(w) * h * 4, "Invalid module output geometry");
    std::ofstream f(static_cast<Output *>(r)->path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (std::size_t i = 0; i < n; i += 4)
        f.write(reinterpret_cast<const char *>(p + i), 3);
    auto *out = static_cast<Output *>(r);
    for (unsigned y = 380; y < 580; ++y)
        for (unsigned x = 330; x < 530; ++x) {
            const auto *pixel = p + (std::size_t(y) * w + x) * 4;
            if (pixel[0] < 160 && pixel[1] > 200 && pixel[2] > 200) {
                out->marker_x += x;
                out->marker_y += y;
                ++out->marker_pixels;
            }
        }
    if (out->marker_pixels) {
        out->marker_x /= out->marker_pixels;
        out->marker_y /= out->marker_pixels;
    }
}
// The Party page's Gear list picture (748 x 1716, transparent below the content): written over the
// page's navy so it can be viewed on its own.
void GearSink(void *r, std::uint32_t w, std::uint32_t h, const std::uint8_t *p, std::size_t n) {
    Check(w == 748 && h == 1716 && n == std::size_t(w) * h * 4, "Invalid gear list geometry");
    std::ofstream f(static_cast<Output *>(r)->path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (std::size_t i = 0; i < n; i += 4)
        for (int c = 0; c < 3; ++c) {
            const int bg = c == 0 ? 14 : c == 1 ? 19 : 33;
            const char v = char((p[i + c] * p[i + 3] + bg * (255 - p[i + 3])) / 255);
            f.write(&v, 1);
        }
}
struct StandaloneImage {
    int w{}, h{};
    std::vector<std::uint8_t> rgba;
};
int main(int argc, char **argv) try {
    Check(argc == 5 || (argc == 6 && (std::string_view(argv[5]) == "stills" ||
                                      std::string_view(argv[5]) == "motion" ||
                                      std::string_view(argv[5]) == "map" ||
                                      std::string_view(argv[5]) == "face" ||
                                      std::string_view(argv[5]) == "battle")),
          "usage: xcx-preview MODULE.SO ROMFS PROBE-ROOT OUT [stills|motion|map|face|battle]");
    const bool motion_mode = argc == 6 && std::string_view(argv[5]) == "motion";
    const bool map_mode = argc == 6 && std::string_view(argv[5]) == "map";
    // `face`: the Skell button's face per helper state word, with the Skell pictures enabled.
    const bool face_mode = argc == 6 && std::string_view(argv[5]) == "face";
    // `battle`: the Battle page's states (on foot, piloting, member down, Skell destroyed, not in
    // battle, values pending, Return blocked, menu open, loading) from the fixture with sample values
    // written into the recorded memory and a fake helper mailbox.
    const bool battle_mode = argc == 6 && std::string_view(argv[5]) == "battle";
    const bool stills_mode = argc == 6 && !motion_mode && !map_mode && !face_mode && !battle_mode;
    // `map` ends with the gesture-burst test, which needs the saved flag area: say so up front.
    Check(!map_mode || std::getenv("XCX_FLAG_AREA"),
          "map mode needs XCX_FLAG_AREA (the saved flag area, e.g. build/xcx-mira/flags/progress.bin)");
    // The unchanged-page replay keeps every page the prototype's; `stills` mode enables the Skell
    // pictures and waits for the background render; `motion` mode drives the HP roll, the picture
    // fade-in, the Reduced motion flag and the idle check.
    if (!stills_mode && !motion_mode && !face_mode)
        setenv("XCX_STILL", "0", 1);
    Host host(argv[2], argv[3], argv[4]);
    fs::create_directories(host.out);
    // The recorded sidecars do not hold the saved records' avatar block. XCX_AVATAR_BLOCK names a
    // 0x90-byte block decoded from the player's own save (runs/20261121-avatar/decode_xcx_save.py,
    // payload 0x48) and is mapped at saved-records root + 0xf8; XCX_AVATAR_NAME renames the avatar
    // actor to match (the fixture's second avatar slot, Terra).
    // The default avatar record is the starter fixture's own (Zanemato, decoded gamedata payload 0x48),
    // so review renders show the real portrait; XCX_AVATAR_BLOCK names another 0x90-byte block.
    static const std::uint8_t default_avatar[0x90] = {0x5a, 0x61, 0x6e, 0x65, 0x6d, 0x61, 0x74, 0x6f, 0x00, 0x61, 0x6d, 0x65, 0x00, 0x49, 0x57, 0x5f, 0x4e, 0x50, 0x5f, 0x46, 0x5f, 0x4b, 0x4e, 0x49, 0x46, 0x45, 0x5f, 0x41, 0x30, 0x36, 0x5f, 0x33, 0x37, 0x30, 0x31, 0x44, 0x52, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x49, 0x57, 0x5f, 0x4e, 0x50, 0x5f, 0x46, 0x5f, 0x4b, 0x4e, 0x49, 0x46, 0x45, 0x5f, 0x41, 0x30, 0x36, 0x5f, 0x34, 0x33, 0x30, 0x08, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x73, 0x00, 0x85, 0x05, 0xf1, 0x05, 0xc7, 0x04, 0x21, 0x05, 0xbf, 0x01, 0x6f, 0x03, 0x00, 0x00, 0xe4, 0xf2, 0x7f, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x01, 0x00, 0x63, 0x00, 0x5c, 0xc1, 0x01, 0x00, 0xab, 0x06, 0x6f, 0x2b, 0xc6, 0x04, 0x00, 0x00, 0x93, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
    bool avatar_block_mapped = false;
    {
        std::vector<std::uint8_t> bytes(default_avatar, default_avatar + sizeof default_avatar);
        if (const char *path = std::getenv("XCX_AVATAR_BLOCK")) {
            std::ifstream f(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), {});
        }
        Check(bytes.size() == 0x90, "XCX_AVATAR_BLOCK must be 0x90 bytes");
        auto *p = host.Find(host.api.main_base + 0x1dc1408, 8);
        Check(p, "No saved-records root pointer in the sidecars");
        std::uint64_t root{};
        std::memcpy(&root, p->bytes.data() + host.api.main_base + 0x1dc1408 - p->address, 8);
        // XCX_SAVE_PAYLOAD: a decoded gamedata payload (decode_xcx_save.py). The four fixture members'
        // character records (payload 0x48 + id * 0x7f8, the profile's first 0x300 bytes) are mapped at
        // root + 0xf8 + id * 0x7f8 for the Party page profile; the avatar's starts with its 0x90 block.
        bool records = false;
        if (const char *path = std::getenv("XCX_SAVE_PAYLOAD")) {
            std::ifstream f(path, std::ios::binary);
            const std::vector<std::uint8_t> save{std::istreambuf_iterator<char>(f), {}};
            for (const unsigned id : {0u, 11u, 12u, 14u}) {
                const std::size_t o = 0x48 + std::size_t(id) * 0x7f8;
                Check(o + 0x300 <= save.size(), "XCX_SAVE_PAYLOAD too short");
                host.blocks.push_back({root + 0xf8 + id * 0x7f8,
                                       std::vector<std::uint8_t>(save.begin() + o, save.begin() + o + 0x300)});
            }
            records = true;
        }
        if (!records)
            host.blocks.push_back({root + 0xf8, std::move(bytes)});
        avatar_block_mapped = true;
    }
    if (const char *rename = std::getenv("XCX_AVATAR_NAME")) {
        const auto actor0 = std::stoull(
            Json::parse(std::ifstream(fs::path(argv[3]) /
                                      "field-baseline/summary.json"))["actors"][0]["actor"]
                .get<std::string>(),
            nullptr, 16);
        auto *b = host.Find(actor0 + 0x248, 0x40);
        Check(b, "Avatar actor name field not in the sidecars");
        std::memset(b->bytes.data() + actor0 + 0x248 - b->address, 0, 0x40);
        std::memcpy(b->bytes.data() + actor0 + 0x248 - b->address, rename, std::strlen(rename));
    }
    // The sidecars do not hold the saved flag area either. XCX_FLAG_AREA names a file of at least
    // 0x2800 bytes (the segment flags cut from a decoded save, see the Map tests) mapped at
    // saved-records root + 0x72e78.
    if (const char *path = std::getenv("XCX_FLAG_AREA")) {
        std::ifstream f(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        Check(bytes.size() >= xcx::FlagBytes, "XCX_FLAG_AREA must hold at least 0x2800 bytes");
        bytes.resize(xcx::FlagBytes);
        auto *p = host.Find(host.api.main_base + 0x1dc1408, 8);
        Check(p, "No saved-records root pointer in the sidecars");
        std::uint64_t root{};
        std::memcpy(&root, p->bytes.data() + host.api.main_base + 0x1dc1408 - p->address, 8);
        host.blocks.push_back({root + xcx::FlagAreaOffset, std::move(bytes)});
    }
    Check(xcx::SupportsBuild(xcx::Build), "Pinned build rejected");
    Check(!xcx::SupportsBuild(xcx::Build.substr(0, 8)), "Short build accepted");
    auto badbuild = std::string(xcx::Build);
    badbuild.back() = '1';
    Check(!xcx::SupportsBuild(badbuild), "Wrong tail build accepted");
    auto s = xcx::Read(host.api);
    Check(s.valid && s.zone == 1401 && s.controlled == 0, "Recorded snapshot rejected");
    Check(s.party[0].pc.hp == 10000 && s.party[1].pc.hp == 11500, "Pilot HP mismatch");
    Check(s.party[0].owned && s.party[0].skell_name == "SE0750 Amdusias TS" &&
              s.party[0].insurance == 2,
          "Owned record mismatch");
    Check(s.party[0].pc.name == (std::getenv("XCX_AVATAR_NAME") ? std::getenv("XCX_AVATAR_NAME")
                                                                 : "Zanemato") &&
              s.party[1].pc.name == "Elma" && s.party[2].pc.name == "Lin" &&
              s.party[3].pc.name == "Irina",
          "Actor names not read from the recorded objects");
    Check(s.avatar.valid == avatar_block_mapped, "Avatar record not (or wrongly) accepted");
    if (avatar_block_mapped) {
        Check(s.avatar.sex == (std::getenv("XCX_AVATAR_NAME") ? 2 : 1) && s.avatar.hair != 0,
              "Avatar record fields mismatch");
        std::cout << "avatar hair=" << s.avatar.hair << " sex=" << s.avatar.sex
                  << " color5=" << s.avatar.color5 << '\n';
        // A record that is not this avatar's (name differs / kind byte) is refused, the snapshot stays.
        auto *rp = host.Find(host.api.main_base + 0x1dc1408, 8);
        std::uint64_t rt{};
        std::memcpy(&rt, rp->bytes.data() + host.api.main_base + 0x1dc1408 - rp->address, 8);
        const auto first = host.Find(rt + 0xf8, 1)->bytes[rt + 0xf8 - host.Find(rt + 0xf8, 1)->address];
        host.Put<std::uint8_t>(rt + 0xf8, 'X');
        auto bad = xcx::Read(host.api);
        Check(bad.valid && !bad.avatar.valid, "Mismatching avatar record accepted");
        host.Put<std::uint8_t>(rt + 0xf8, first);
        Check(xcx::Read(host.api).avatar.valid, "Avatar record not restored");
    }
    Check(s.party[1].insurance == 3 && s.party[2].frame == 156 && !s.party[3].owned,
          "Assignment mismatch");
    auto *ptr = host.Find(host.api.main_base + 0x1dc1408, 8);
    std::uint64_t root{};
    std::memcpy(&root, ptr->bytes.data() + host.api.main_base + 0x1dc1408 - ptr->address, 8);
    host.Put<std::uint8_t>(root + 0x8ab, 255);
    Check(!xcx::Read(host.api).party[0].owned, "Invalid assignment accepted");
    host.Put<std::uint8_t>(root + 0x8ab, 1);
    // Unknown status classes and malformed current/max values invalidate the snapshot.
    const auto object =
        std::stoull(Json::parse(std::ifstream(fs::path(argv[3]) /
                                              "field-baseline/summary.json"))["actors"][0]["status"]
                        .get<std::string>(),
                    nullptr, 16);
    const auto oldvt = host.Find(object, 8);
    std::uint64_t vt{};
    std::memcpy(&vt, oldvt->bytes.data() + object - oldvt->address, 8);
    host.Put<std::uint64_t>(object, 0);
    Check(!xcx::Read(host.api).valid, "Unknown status accepted");
    host.Put(object, vt);
    host.Put<std::int32_t>(object + 0x250, -1);
    Check(!xcx::Read(host.api).valid, "Negative HP accepted");
    host.Put<std::int32_t>(object + 0x250, 10000);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    Check(lib, dlerror() ? "Module load failed" : "Module load failed");
    auto get = reinterpret_cast<EdenDsmodGetModuleFn>(dlsym(lib, "eden_dsmod_get_module"));
    auto extget =
        reinterpret_cast<EdenDsmodGetExtensionsFn>(dlsym(lib, "eden_dsmod_get_extensions"));
    Check(get && extget, "Module exports missing");
    Check(!get(0, 0), "Wrong ABI accepted");
    const auto *api = get(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto *ext = extget(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    Check(api && ext, "ABI negotiation failed");
    Check(api->supports_build(std::string(xcx::Build).c_str()), "Module build rejected");
    void *module = api->create(&host.api, "{}");
    Check(module, "Module create failed");
    if (face_mode) {
        // The helper through a fake mailbox, a state word per case, the Map page rendered and the
        // button's plate checked: no plate for the disabled faces, a teal plate for Board / Exit /
        // Return. Regression for the Return face reading greyed while the state word said 0x1fc8d.
        struct FakeMailbox {
            std::array<std::uint32_t, 1024> w{};
        } mailbox;
        EdenDsmodHostExtensions hx{};
        hx.version = EDEN_DSMOD_EXT_VERSION;
        hx.struct_size = sizeof hx;
        hx.abi_hash = EDEN_DSMOD_EXT_HASH;
        hx.userdata = &mailbox;
        hx.mailbox_address = 0x7000;
        hx.mailbox_size = 4096;
        hx.load_u32 = [](void *u, std::uint32_t off, std::uint32_t *v) -> EdenDsmodBool {
            if (off % 4 || off >= 4096)
                return false;
            *v = static_cast<FakeMailbox *>(u)->w[off / 4];
            return true;
        };
        hx.store_u32 = [](void *u, std::uint32_t off, std::uint32_t v) -> EdenDsmodBool {
            if (off % 4 || off >= 4096)
                return false;
            static_cast<FakeMailbox *>(u)->w[off / 4] = v;
            return true;
        };
        namespace xh = xcx_helper;
        const auto word = [&](std::uint32_t off) -> std::uint32_t & { return mailbox.w[off / 4]; };
        mailbox.w[xh::EpochOffset / 4] = 5;
        ext->configure(module, &hx);
        std::uint32_t state = 0;
        int calls = 0;
        const auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                word(xh::OffsetGuestMagic) = xh::GuestMagic;
                ++word(xh::OffsetHeartbeat);
                if (++calls % 5 == 0) {
                    word(xh::OffsetState) = state;
                    ++word(xh::OffsetPollBeat);
                }
                api->sample(module, &host.api);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
        };
        const auto plate = [&](const fs::path &path) {
            std::ifstream f(path, std::ios::binary);
            std::string header;
            std::getline(f, header);
            std::getline(f, header);
            std::getline(f, header);
            std::vector<char> px(1240 * 1080 * 3);
            f.read(px.data(), px.size());
            const auto *p = reinterpret_cast<const unsigned char *>(px.data()) +
                            (std::size_t(842) * 1240 + 930) * 3;
            return p[1] > 60 && p[2] > 65 && p[0] < 50; // the 0x1f6f7a99 fill over the panel
        };
        // The live companion shows the Skell picture: wait for it, as the live run did.
        frames(30);
        const auto started = std::chrono::steady_clock::now();
        while (host.ints["xcx.still_ready"] != 1) {
            frames(5);
            Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(120),
                  "Skell still never landed");
        }
        frames(60);
        const std::uint32_t base = xh::StateValid | xh::StateGate | xh::StateHudOk |
                                   xh::StateInputReady | xh::StateFieldOk;
        const std::uint32_t on_foot = base | xh::StateOnFoot | xh::StateRecFoot | xh::StateIdle |
                                      xh::StateCooldownClear | xh::StateSwitchReady;
        struct Case {
            const char *name;
            std::uint32_t state;
            int face_kind; // xcx.face_kind: 0 prototype, 1 Board, 2 Exit, 3 Return
            bool plate;
        };
        const Case cases[] = {
            {"disabled-no-bits", on_foot, 0, false},
            {"board", on_foot | xh::StateCanBoard, 1, true},
            {"exit", base | xh::StatePiloting | xh::StateCanExit, 2, true},
            {"return-0x1fc8d", 0x1fc8d, 3, true},
            {"disabled-menu", on_foot & ~xh::StateInputReady, 0, false},
            {"disabled-loading", on_foot & ~xh::StateFieldOk, 0, false},
        };
        for (const auto &c : cases) {
            state = c.state;
            frames(80);
            Check(ext->on_action(module, "page", 0), "Page action refused");
            frames(80);
            Check(host.ints["xcx.face_kind"] == c.face_kind, c.name);
            Output output{host.out / (std::string(c.name) + ".ppm")};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &output, Sink),
                  "Render failed");
            const bool shown = plate(output.path);
            std::cout << c.name << " face_kind=" << host.ints["xcx.face_kind"] << " plate=" << shown
                      << '\n';
            Check(shown == c.plate, "Button plate does not match the state word");
        }
        state = 0x1fc8d;
        frames(80);
        Check(ext->on_action(module, "page", 0), "Page action refused");
        frames(40);
        Check(!ext->on_action(module, "skell_button", 0), "A short tap acted on the Return face");
        frames(10);
        Check(host.ints["xcx.return_hint"] == 1, "No Hold hint");
        Output hint{host.out / "return-hold-hint.ppm"};
        Check(ext->load_image(module, &host.api, host.frame.c_str(), &hint, Sink), "Render failed");
        Check(!host.writes, "Face preview attempted a game write");
        api->destroy(module);
        dlclose(lib);
        std::cout << "Skell button faces rendered through the module ABI.\n";
        return 0;
    }
    if (battle_mode) {
        // The Battle page through the module ABI. Sample values (the battle-view mockups' numbers)
        // are written into the recorded memory; the helper is a fake mailbox. Nothing here is a game
        // write: the host's write_memory stays unused.
        struct FakeMailbox {
            std::array<std::uint32_t, 1024> w{};
        } mailbox;
        EdenDsmodHostExtensions hx{};
        hx.version = EDEN_DSMOD_EXT_VERSION;
        hx.struct_size = sizeof hx;
        hx.abi_hash = EDEN_DSMOD_EXT_HASH;
        hx.userdata = &mailbox;
        hx.mailbox_address = 0x7000;
        hx.mailbox_size = 4096;
        hx.load_u32 = [](void *u, std::uint32_t off, std::uint32_t *v) -> EdenDsmodBool {
            if (off % 4 || off >= 4096)
                return false;
            *v = static_cast<FakeMailbox *>(u)->w[off / 4];
            return true;
        };
        hx.store_u32 = [](void *u, std::uint32_t off, std::uint32_t v) -> EdenDsmodBool {
            if (off % 4 || off >= 4096)
                return false;
            static_cast<FakeMailbox *>(u)->w[off / 4] = v;
            return true;
        };
        namespace xh = xcx_helper;
        const auto word = [&](std::uint32_t off) -> std::uint32_t & { return mailbox.w[off / 4]; };
        mailbox.w[xh::EpochOffset / 4] = 5;
        ext->configure(module, &hx);
        std::uint32_t state = 0;
        int calls = 0;
        const auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                word(xh::OffsetGuestMagic) = xh::GuestMagic;
                ++word(xh::OffsetHeartbeat);
                if (++calls % 5 == 0) {
                    word(xh::OffsetState) = state;
                    ++word(xh::OffsetPollBeat);
                }
                api->sample(module, &host.api);
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        };
        // The recorded actors (slot, role) -> object and status addresses.
        const auto summary = Json::parse(std::ifstream(fs::path(argv[3]) / "field-baseline/summary.json"));
        std::array<std::uint64_t, 4> pc_obj{}, pc_status{}, sk_obj{}, sk_status{};
        for (const auto &a : summary["actors"]) {
            if (a["state"] != "resolved")
                continue;
            const int slot = a["slot"];
            const auto obj = std::stoull(a["actor"].get<std::string>(), nullptr, 16);
            const auto st = std::stoull(a["status"].get<std::string>(), nullptr, 16);
            (a["role"] == "pc" ? pc_obj : sk_obj)[slot] = obj;
            (a["role"] == "pc" ? pc_status : sk_status)[slot] = st;
        }
        const auto party = host.api.main_base + 0x1df58d0;
        const auto set_hp = [&](std::uint64_t status, std::int32_t hp) { host.Put<std::int32_t>(status + 0x250, hp); };
        const auto set_points = [&](std::uint64_t status, std::int32_t p) { host.Put<std::int32_t>(status + 0x268, p); };
        const auto driven = [&](std::uint64_t obj, bool on) {
            auto *b = host.Find(obj + 0xd0, 1);
            Check(b, "No actor +0xd0 in the sidecars");
            auto &byte = b->bytes[obj + 0xd0 - b->address];
            byte = on ? (byte | 0x20) : (byte & ~0x20);
        };
        const auto mounted = [&](bool on) { // party record byte +10 of the three Skell-owning slots
            for (int slot = 0; slot < 3; ++slot)
                host.Put<std::uint8_t>(party + 0xc + 0x14 * slot + 10, on ? 0 : 1);
            driven(pc_obj[0], !on);
            driven(sk_obj[0], on);
        };
        // The mockups' sample values (fixture maxima stay: 10000 / 11500 / 14000 / 8000, Skell 20400).
        const std::int32_t hp[4] = {7420, 11500, 12610, 6965}, tp[4] = {1840, 2410, 1250, 960};
        const auto reset = [&] {
            for (int i = 0; i < 4; ++i) {
                set_hp(pc_status[i], hp[i]);
                set_points(pc_status[i], tp[i]);
            }
            set_hp(sk_status[0], 20298);
            set_points(sk_status[0], 1200);
            set_hp(sk_status[1], 16935);
            set_points(sk_status[1], 900);
            set_hp(sk_status[2], 14966);
            set_points(sk_status[2], 600);
            host.Put<float>(sk_status[0] + 0x3cf0, 24952.f);
            mounted(false);
            unsetenv("XCX_BATTLE_FLAG");
        };
        const std::uint32_t base = xh::StateValid | xh::StateGate | xh::StateHudOk | xh::StateInputReady |
                                   xh::StateFieldOk;
        const std::uint32_t on_foot = base | xh::StateOnFoot | xh::StateRecFoot | xh::StateIdle |
                                      xh::StateCooldownClear | xh::StateSwitchReady;
        const auto pixel = [](const fs::path &path, int x, int y) {
            std::ifstream f(path, std::ios::binary);
            std::string line;
            std::getline(f, line);
            std::getline(f, line);
            std::getline(f, line);
            std::vector<unsigned char> px(1240 * 1080 * 3);
            f.read(reinterpret_cast<char *>(px.data()), px.size());
            const auto *p = px.data() + (std::size_t(y) * 1240 + x) * 3;
            return std::array<int, 3>{p[0], p[1], p[2]};
        };
        const auto teal = [&](const fs::path &path, int x, int y) { // the 0x1f6f7a99 enabled fill
            const auto p = pixel(path, x, y);
            return p[1] > 60 && p[2] > 65 && p[0] < 50;
        };
        struct Case {
            const char *name;
            std::function<void()> setup;
            std::uint32_t state;
            int select;      // bpick target, -1 none
            int face_kind;   // xcx.face_kind
            bool commands;   // xcx.battle_cmd_ok
            bool set_on;     // the Set plate's teal fill
            bool button_on;  // the Skell button's teal fill
        };
        const std::vector<Case> cases{
            {"battle-on-foot", [] {}, on_foot | xh::StateCanBoard, -1, 1, true, false, true},
            {"battle-piloting", [&] { mounted(true); }, base | xh::StatePiloting | xh::StateCanExit, -1, 2, true,
             false, true},
            {"member-down", [&] { set_hp(pc_status[3], 0); }, on_foot | xh::StateCanBoard, 1, 1, true, true, true},
            {"skell-destroyed",
             [&] {
                 mounted(true);
                 set_hp(sk_status[0], 0);
                 set_points(sk_status[0], 0);
             },
             base | xh::StatePiloting, -1, 2, true, false, false},
            {"not-in-battle", [&] { setenv("XCX_BATTLE_FLAG", "out", 1); },
             on_foot | xh::StateCanReturn, -1, 3, true, false, true},
            {"values-pending", [&] { mounted(true); }, base | xh::StatePiloting | xh::StateCanExit, -1, 2, true,
             false, true},
            // Return mid-battle on the Battle page: a hold acts once tested live (ReturnMidBattleLive), else
            // the plate is grey and refused.
            {"return-in-battle", [&] { setenv("XCX_BATTLE_FLAG", "in", 1); }, on_foot | xh::StateCanReturn, -1, 3,
             true, false, xcx::battle::ReturnMidBattleLive},
            {"native-menu-open", [] {}, (on_foot | xh::StateCanBoard) & ~xh::StateInputReady, -1, 0, false, false,
             false},
            {"loading", [] {}, (on_foot | xh::StateCanBoard) & ~xh::StateFieldOk, -1, 0, false, false, false},
            // Elma (slot 1) still rides her Skell while the controlled member is on foot: Set is refused
            // with "Not available now" (BATTLE-LIVE-2, BUG 1).
            {"set-mounted-refused", [&] { host.Put<std::uint8_t>(party + 0xc + 0x14 * 1 + 10, 0); },
             on_foot | xh::StateCanBoard, 1, 1, true, false, true},
            // An on-foot member picked while the in-combat flag is In: Set is off during a battle (owner
            // 2026-11-22: a switch mid-battle ended the fight and left the stick dead ~1 min). Kept last: the
            // automatic view's leave debounce keeps it off for 2 s after the flag drops.
            {"set-in-battle-refused", [&] { setenv("XCX_BATTLE_FLAG", "in", 1); }, on_foot | xh::StateCanBoard, 1, 1,
             true, false, true},
        };
        frames(20);
        Check(ext->on_action(module, "page", 4), "Battle page refused");
        for (const auto &c : cases) {
            reset();
            c.setup();
            state = c.state;
            Check(ext->on_action(module, "pick", 0), "Reset selection refused"); // Party page selection
            Check(ext->on_action(module, "page", 4), "Battle page refused");
            frames(60);
            if (c.select >= 0)
                Check(ext->on_action(module, "bpick", c.select), "Battle card selection refused");
            frames(60);
            Check(host.ints["xcx.page"] == 4, "bpick left the Battle page");
            Check(host.ints["xcx.face_kind"] == c.face_kind, c.name);
            Check(host.ints["xcx.battle_cmd_ok"] == c.commands, "Battle Commands gate");
            Output output{host.out / (std::string(c.name) + ".ppm")};
            const auto render_start = std::chrono::steady_clock::now();
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &output, Sink), "Battle render failed");
            const double render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                render_start)
                                         .count();
            const bool cmd = teal(output.path, 476, 720), set = teal(output.path, 40, 836),
                       btn = teal(output.path, 930, 842);
            std::cout << c.name << " face_kind=" << host.ints["xcx.face_kind"] << " commands=" << cmd
                      << " set=" << set << " button=" << btn << " render_ms=" << render_ms << '\n';
            Check(cmd == c.commands, "Battle Commands plate does not match its gate");
            Check(set == c.set_on, "Set plate does not match the selection / helper");
            Check(btn == c.button_on, "Skell button plate does not match the helper face");
            if (std::string_view(c.name) == "set-mounted-refused") {
                Check(host.ints["xcx.dbg.set_blocked"] == 1 && host.ints["xcx.set_enabled"] == 0,
                      "Set on a mounted member not blocked");
                Check(!ext->on_action(module, "switch_set", 0), "Set on a mounted member accepted");
            }
            if (std::string_view(c.name) == "member-down")
                Check(host.ints["xcx.dbg.set_blocked"] == 0 && host.ints["xcx.dbg.set_in_battle"] == 0,
                      "Set on an on-foot member blocked");
            if (std::string_view(c.name) == "set-in-battle-refused") {
                Check(host.ints["xcx.dbg.set_in_battle"] == 1 && host.ints["xcx.dbg.set_blocked"] == 1 &&
                          host.ints["xcx.set_enabled"] == 0,
                      "Set not blocked in battle");
                Check(!ext->on_action(module, "switch_set", 0), "Set accepted in battle (Battle page)");
                // The Party page: same refusal.
                Check(ext->on_action(module, "pick", 1), "Party pick refused");
                frames(40);
                Check(host.ints["xcx.page"] == 1 && host.ints["xcx.set_enabled"] == 0 &&
                          host.ints["xcx.dbg.set_blocked"] == 1,
                      "Party page Set not blocked in battle");
                Check(!ext->on_action(module, "switch_set", 0), "Set accepted in battle (Party page)");
                Output party_out{host.out / "set-in-battle-party.ppm"};
                Check(ext->load_image(module, &host.api, host.frame.c_str(), &party_out, Sink), "Party render failed");
                // The flag drops: still off inside the 2 s leave debounce, on again after it.
                setenv("XCX_BATTLE_FLAG", "out", 1);
                frames(60);
                Check(host.ints["xcx.dbg.set_in_battle"] == 1, "Set back on before the leave debounce");
                // The earlier cases flip the flag often, so the flicker escalation (6 s) may apply: wait for the
                // debounced battle state to clear, then one more sample for the Set gate.
                for (int k = 0; k < 120 && host.ints["xcx.battle"] != 0; ++k)
                    frames(10);
                Check(host.ints["xcx.battle"] == 0, "battle state did not clear");
                frames(60);
                Check(host.ints["xcx.dbg.set_in_battle"] == 0 && host.ints["xcx.set_enabled"] == 1,
                      "Set not back on after the battle");
                Output after_out{host.out / "set-after-battle-party.ppm"};
                Check(ext->load_image(module, &host.api, host.frame.c_str(), &after_out, Sink), "Party render failed");
                std::cout << "set in battle: refused on Battle and Party, back after the debounce\n";
                unsetenv("XCX_BATTLE_FLAG");
            }
            if (std::string_view(c.name) == "return-in-battle") {
                if (xcx::battle::ReturnMidBattleLive) {
                    Check(ext->on_action(module, "return_hold", 0), "Return hold refused on the Battle page");
                } else {
                    Check(!ext->on_action(module, "return_hold", 0), "Return hold accepted on the Battle page");
                    Check(!ext->on_action(module, "skell_button", 0), "Return tap accepted on the Battle page");
                }
            }
        }
        // The Map page's right rail (owner 2026-11-27): a row tap switches to that member through the Set
        // plate's helper path and gates, without leaving the Map. Refusals first: a staged switch stays
        // pending here (the fake guest never answers).
        {
            const auto staged = [&] { return word(xh::OffsetReqSeq); };
            const auto rail_ok = [&](int row) {
                return host.ints["xcx.rail_tap" + std::to_string(row)] == 1;
            };
            reset();
            state = on_foot | xh::StateCanBoard;
            Check(ext->on_action(module, "page", 0), "Map page refused");
            frames(60);
            Check(!rail_ok(0) && rail_ok(1) && rail_ok(2), "rail tap targets: controlled row off, others on");
            const auto seq0 = staged();
            // the controlled member: nothing happens (its row has no tap target; a raced tap is accepted as a no-op)
            Check(ext->on_action(module, "rail_switch", 0), "controlled row tap refused");
            // a finger on the map's pan / pinch: no row takes a tap while it is down and shortly after
            host.view_i64["@view_touch_down:xcx-map-world"] = 1;
            frames(5);
            Check(!rail_ok(1) && !rail_ok(2), "rail rows tappable during a map gesture");
            host.view_i64.clear();
            frames(40);
            Check(rail_ok(1), "rail rows not back after the map gesture");
            // a mounted target (Elma rides her Skell): refused, hint on her row, still the Map page
            host.Put<std::uint8_t>(party + 0xc + 0x14 * 1 + 10, 0);
            frames(20);
            Check(!ext->on_action(module, "rail_switch", 1), "rail switch to a mounted member accepted");
            frames(20);
            Check(host.ints["xcx.page"] == 0, "rail tap left the Map page");
            Output mounted_out{host.out / "map-rail-refused.ppm"};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &mounted_out, Sink), "rail render failed");
            const auto hint_key = host.frame;
            reset();
            frames(20);
            // a native menu (input not accepted): refused
            state = (on_foot | xh::StateCanBoard) & ~xh::StateInputReady;
            frames(40);
            Check(!ext->on_action(module, "rail_switch", 1), "rail switch accepted under a native menu");
            state = on_foot | xh::StateCanBoard;
            // a battle (party-wide flag): refused; the Map tab tap keeps the automatic view away
            setenv("XCX_BATTLE_FLAG", "in", 1);
            frames(40);
            Check(ext->on_action(module, "page", 0), "Map page refused");
            frames(20);
            Check(host.ints["xcx.page"] == 0, "Map page left in battle");
            Check(!ext->on_action(module, "rail_switch", 2), "rail switch accepted in battle");
            setenv("XCX_BATTLE_FLAG", "out", 1);
            for (int k = 0; k < 120 && host.ints["xcx.battle"] != 0; ++k)
                frames(10);
            frames(60);
            unsetenv("XCX_BATTLE_FLAG");
            // the hint goes away after RailNoteMs (1.5 s)
            for (int k = 0; k < 40 && host.frame == hint_key; ++k)
                frames(10);
            Check(staged() == seq0, "a refused rail tap staged a switch");
            // allowed: staged as the Set plate's switch (slot 1, Elma's token), the Map stays
            Check(ext->on_action(module, "page", 0), "Map page refused");
            frames(40);
            Check(ext->on_action(module, "rail_switch", 1), "rail switch refused in the field");
            Check(staged() != seq0 && word(xh::OffsetReqOp) == std::uint32_t(xh::Op::Switch) &&
                      word(xh::OffsetReqArg) == 1,
                  "rail switch did not stage the helper's switch for slot 1");
            frames(20);
            Check(host.ints["xcx.page"] == 0, "rail switch left the Map page");
            // a second tap while that one is in flight: refused (Working / cooldown)
            Check(!ext->on_action(module, "rail_switch", 2), "a second rail switch was accepted while one is pending");
            Output ok_out{host.out / "map-rail-pending.ppm"};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &ok_out, Sink), "rail render failed");
            std::cout << "map rail: controlled no-op, gesture gate, mounted / menu / battle / pending refused, switch staged\n";
            // the pending request blocks later cases: start them with a fresh mailbox epoch
            mailbox.w = {};
            mailbox.w[xh::EpochOffset / 4] = 6;
            ext->configure(module, &hx);
        }
        // The rail's layouts (2026-11-27 polish): 4, 3, 2 and 1 members, with the Skell revealed and before it
        // (every Skell handle cleared), and the press window of a row tap (xcx.rail_press, the manifest's
        // flash). Members leave from the last slot, as the game's party does; the fixture is restored after.
        {
            reset();
            state = on_foot | xh::StateCanBoard;
            std::array<std::uint32_t, 8> handles{};
            for (int k = 0; k < 8; ++k) {
                const auto at = party + 0xc + 0x14 * (k / 2) + 4 * (k % 2);
                auto *b = host.Find(at, 4);
                Check(b, "No party record in the sidecars");
                std::memcpy(&handles[k], b->bytes.data() + (at - b->address), 4);
            }
            const auto layout = [&](int members, bool skell) {
                for (int k = 0; k < 8; ++k) {
                    const bool keep = k / 2 < members && (skell || k % 2 == 0);
                    host.Put<std::uint32_t>(party + 0xc + 0x14 * (k / 2) + 4 * (k % 2), keep ? handles[k] : 0);
                }
            };
            for (const bool skell : {true, false})
                for (int members = 4; members >= 1; --members) {
                    layout(members, skell);
                    frames(30);
                    Check(ext->on_action(module, "page", 0), "Map page refused");
                    frames(30);
                    Check(host.ints["xcx.has_skell"] == int(skell), "rail layout: has_skell");
                    const auto name = "map-rail-" + std::to_string(members) + (skell ? "-skell" : "-noskell");
                    Output out{host.out / (name + ".ppm")};
                    Check(ext->load_image(module, &host.api, host.frame.c_str(), &out, Sink), "rail render failed");
                    std::cout << name << " rail_tap " << host.ints["xcx.rail_tap1"] << host.ints["xcx.rail_tap2"]
                              << host.ints["xcx.rail_tap3"] << '\n';
                }
            // the press window: a tap publishes its row on xcx.rail_press, then -1 again (RailPressMs)
            layout(4, false);
            frames(30);
            Check(ext->on_action(module, "page", 0), "Map page refused");
            frames(30);
            Check(host.ints["xcx.rail_press"] == -1, "rail press published at rest");
            ext->on_action(module, "rail_switch", 2);
            api->sample(module, &host.api);
            Check(host.ints["xcx.rail_press"] == 2, "rail press not published after a row tap");
            frames(20);
            Check(host.ints["xcx.rail_press"] == -1, "rail press still published after RailPressMs");
            layout(4, true);
            mailbox.w = {};
            mailbox.w[xh::EpochOffset / 4] = 7;
            ext->configure(module, &hx);
            frames(30);
            std::cout << "map rail layouts: 1-4 members with and without a Skell, press window\n";
        }
        // Frame pacing (BATTLE-LIVE-2): while the last queued picture has not been rendered, a value
        // change queues no new one (the host renders in order; a backlog made the page trail the game).
        {
            reset();
            state = on_foot | xh::StateCanBoard;
            Check(ext->on_action(module, "page", 4), "Battle page refused");
            frames(40);
            Output first{host.out / "pacing-0.ppm"};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &first, Sink), "pacing render failed");
            set_hp(pc_status[3], 5000);
            frames(15);
            const std::string queued = host.frame;
            set_hp(pc_status[3], 4000);
            frames(15);
            Check(host.frame == queued, "a new Battle picture was queued before the last one was rendered");
            Output second{host.out / "pacing-1.ppm"};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &second, Sink), "pacing render failed");
            frames(15);
            Check(host.frame != queued, "the newest values were not queued after the render");
            std::cout << "battle frame pacing ok\n";
        }
        // The other pages keep their own content with the 5-slot tab bar.
        reset();
        state = on_foot | xh::StateCanBoard;
        for (const auto &[name, page] : {std::pair{"tabs-map", 0}, {"tabs-party", 1}, {"tabs-skell", 2},
                                         {"tabs-settings", 3}}) {
            Check(ext->on_action(module, "page", page), "Page action refused");
            frames(40);
            Output output{host.out / (std::string(name) + ".ppm")};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &output, Sink), "Page render failed");
            std::cout << name << '\n';
        }
        Check(!host.writes, "Battle preview attempted a game write");
        api->destroy(module);
        dlclose(lib);
        std::cout << "Battle page states rendered through the module ABI.\n";
        return 0;
    }
    if (map_mode) {
        // The Map page's native widgets, driven through the module ABI like the runtime does: the fixture's
        // zone record, pilot position and the mode flag are rewritten in the (private, in-memory) sidecars;
        // the runtime's view of a map widget (@map_view_*) is simulated for pan / pinch. Writes, per case,
        // <name>.json (the published values the widgets bind to), <name>-page.pam (the page picture with the
        // map hole) and the world pictures the visible overlays would load (<name>-<set>-<n>.pam).
        const auto actor0 =
            std::stoull(Json::parse(std::ifstream(fs::path(argv[3]) /
                                                  "field-baseline/summary.json"))["actors"][0]["actor"]
                            .get<std::string>(),
                        nullptr, 16);
        auto *zp = host.Find(host.api.main_base + 0x1df5968, 8);
        Check(zp, "No zone pointer in the sidecars");
        std::uint64_t zone_record{};
        std::memcpy(&zone_record, zp->bytes.data() + host.api.main_base + 0x1df5968 - zp->address, 8);
        const auto write_pam = [](const fs::path &path, std::uint32_t w, std::uint32_t h, const std::uint8_t *p) {
            std::ofstream f(path, std::ios::binary);
            f << "P7\nWIDTH " << w << "\nHEIGHT " << h << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
            f.write(reinterpret_cast<const char *>(p), std::streamsize(w) * h * 4);
        };
        struct Pam {
            fs::path path;
            std::uint32_t w{}, h{};
            std::vector<std::uint8_t> rgba;
        };
        const auto sink = [](void *r, std::uint32_t w, std::uint32_t h, const std::uint8_t *p, std::size_t n) {
            auto *pam = static_cast<Pam *>(r);
            Check(n == std::size_t(w) * h * 4, "Invalid picture geometry");
            pam->w = w;
            pam->h = h;
            pam->rgba.assign(p, p + n);
        };
        double slowest_picture_ms = 0;
        const auto request = [&](const std::string &key, const fs::path &path) {
            Pam pam{path};
            const auto t0 = std::chrono::steady_clock::now();
            Check(ext->load_image(module, &host.api, key.c_str(), &pam, sink), "Map picture refused");
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            slowest_picture_ms = std::max(slowest_picture_ms, ms);
            if (ms > 1500)
                std::cout << "  slow picture " << key << ": " << ms << " ms\n";
            Check(std::size_t(pam.w) * pam.h * 4 <= 16u * 1024 * 1024 && pam.w <= 4096 && pam.h <= 4096, "Picture over the runtime's limits");
            write_pam(path, pam.w, pam.h, pam.rgba.data());
            return pam;
        };
        const auto opaque_fraction = [](const Pam &pam) {
            std::size_t n = 0;
            for (std::size_t i = 3; i < pam.rgba.size(); i += 4)
                n += pam.rgba[i] > 128;
            return double(n) / (double(pam.rgba.size()) / 4);
        };
        struct Scenario {
            const char *name;
            std::uint32_t zone;
            const char *area;
            float x, z;
            int mode;
            // simulated widget view (the runtime's drawn view, world centre and zoom); ppw 0 = none yet
            double view_x, view_y, view_ppw;
            const char *view_area;
        };
        const double follow_world = 860. / 2400., follow_nla = 860. / 520.;
        const Scenario scenarios[] = {
            {"nla-overworld", 1401, "Administrative District", -1470.28f, 7468.51f, 0, 0, 0, 0, ""},
            {"nla-area", 1401, "Administrative District", -1470.28f, 7468.51f, 1, -1470.28, -7468.51, follow_nla, "xcx_nla"},
            {"primordia-overworld", 1501, "Talon Rock Summit", -2495.74f, 5748.33f, 0, 0, 0, 0, ""},
            {"primordia-area", 1501, "Talon Rock Summit", -2495.74f, 5748.33f, 1, -2495.74, -5748.33, follow_world, "xcx_area"},
            // the user dragged the view 6 km east of the player: the pictures it needs follow it
            {"primordia-area-dragged-east", 1501, "Talon Rock Summit", -2495.74f, 5748.33f, 1, 3500., -5748.33, follow_world, "xcx_area"},
            // ... and pinched out to a 7 km window
            {"primordia-area-pinched-out", 1501, "Talon Rock Summit", -2495.74f, 5748.33f, 1, -2495.74, -5748.33, follow_world * 0.35, "xcx_area"},
            {"primordia-area-pinched-mid", 1501, "Talon Rock Summit", -2495.74f, 5748.33f, 1, -2495.74, -5748.33, follow_world * 0.6, "xcx_area"},
            {"noctilum-overworld", 1601, "Noctilum", -6577.31f, 1590.17f, 0, 0, 0, 0, ""},
            {"noctilum-area", 1601, "Noctilum", -6577.31f, 1590.17f, 1, -6577.31, -1590.17, follow_world, "xcx_area"},
            {"barracks-overworld", 5000, "BLADE Barracks", 0.f, 0.f, 0, 0, 0, 0, ""},
            {"barracks-area", 5000, "BLADE Barracks", 0.f, 0.f, 1, 0, 0, 0, ""},
            {"barracks-area-east", 5000, "BLADE Barracks", 20.5f, 6.f, 1, 0, 0, 0, ""},
            {"no-map-area-area", 3000, "Lifehold Core", 0.f, 0.f, 1, 0, 0, 0, ""},
        };
        const char *names[] = {"over", "world", "nla", "barracks", "none", "home"};
        const bool burst_only = std::getenv("XCX_BURST_ONLY") != nullptr; // stress runs: skip the scenario sweep
        for (const auto &sc : scenarios) {
            if (burst_only)
                break;
            host.Put<std::uint32_t>(zone_record + 0xac, sc.zone);
            auto *nb = host.Find(zone_record + 0xb8, 0x48);
            Check(nb, "Zone name field not in the sidecars");
            std::memset(nb->bytes.data() + zone_record + 0xb8 - nb->address, 0, 0x48);
            std::memcpy(nb->bytes.data() + zone_record + 0xb8 - nb->address, sc.area, std::strlen(sc.area));
            host.Put<float>(actor0 + 0x50, sc.x);
            host.Put<float>(actor0 + 0x58, sc.z);
            host.flags["xcx.map_mode"] = sc.mode;
            host.view_f64.clear();
            if (sc.view_ppw > 0) {
                const std::string a = sc.view_area;
                host.view_f64["@map_view_cx:" + a] = sc.view_x;
                host.view_f64["@map_view_cy:" + a] = sc.view_y;
                host.view_f64["@map_view_ppw:" + a] = sc.view_ppw;
                host.view_f64["@map_view_w:" + a] = 860;
                host.view_f64["@map_view_h:" + a] = 960;
            }
            Check(ext->on_action(module, "page", 0), "Page action refused");
            for (int i = 0; i < 4; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(130));
                api->sample(module, &host.api);
            }
            // The world pictures are built on the module's own thread and shown once built: wait for them.
            {
                const bool lo_wanted = sc.mode == 0 || sc.zone == 1501 || sc.zone == 1601;
                const auto t0 = std::chrono::steady_clock::now();
                int last = -1, stable = 0;
                while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(240) && stable < 12) {
                    api->sample(module, &host.api);
                    int count = 0;
                    for (int n = 0; n < 4; ++n)
                        count += host.ints["xcx.mls." + std::to_string(n)];
                    for (int n = 0; n < 36; ++n)
                        count += host.ints["xcx.mhs." + std::to_string(n)];
                    for (int n = 0; n < 16; ++n)
                        count += host.ints["xcx.mns." + std::to_string(n)];
                    int lo = 0;
                    for (int n = 0; n < 4; ++n)
                        lo += host.ints["xcx.mls." + std::to_string(n)];
                    stable = count == last && (lo == 4 || !lo_wanted) ? stable + 1 : 0;
                    last = count;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            // The cost of the sample path (what the host's tick pays): back-to-back samples, then samples
            // spaced past the 100 ms read cadence so some include the memory read.
            {
                using Clk = std::chrono::steady_clock;
                double sum = 0, worst = 0;
                const int n = 300;
                for (int i = 0; i < n; ++i) {
                    const auto t0 = Clk::now();
                    api->sample(module, &host.api);
                    const double ms = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
                    sum += ms;
                    worst = std::max(worst, ms);
                }
                double read_worst = 0;
                for (int i = 0; i < 8; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(105));
                    const auto t0 = Clk::now();
                    api->sample(module, &host.api);
                    read_worst = std::max(read_worst, std::chrono::duration<double, std::milli>(Clk::now() - t0).count());
                }
                std::cout << "  sample cost " << sc.name << ": mean " << sum / n << " ms, max " << worst
                          << " ms back-to-back, max " << read_worst << " ms with the read\n";
                Check(worst < 4. && read_worst < 4., "The sample path must stay under 4 ms");
            }
            // The widget gates (exactly one, matching the mode and the area).
            int gates = 0, gate = -1;
            const char *gate_names[] = {"xcx.mw.over", "xcx.mw.world", "xcx.mw.nla", "xcx.mw.barracks", "xcx.mw.none", "xcx.mw.home"};
            for (int g = 0; g < 6; ++g)
                if (host.ints[gate_names[g]]) {
                    ++gates;
                    gate = g;
                }
            Check(gates == 1, "Exactly one native map widget must show");
            const bool over = sc.mode == 0;
            const bool at_home = over && !(sc.zone == 1401 || sc.zone == 1501 || sc.zone == 1601);
            const int expect = over ? (at_home ? 5 : 0) : sc.zone == 1401 ? 2 : sc.zone == 5000 ? 3 : sc.zone == 3000 ? 4 : 1;
            Check(gate == expect, "Wrong widget for the mode and area");
            Check(host.ints["xcx.map_mode"] == sc.mode, "Mode not published");
            // The live marker: world x and y = -z, only where the area has a position; the heading is not
            // read yet (the arrow points up, said so).
            const bool barracks = sc.zone == 5000 && sc.mode == 1; // Area tab only: the Barracks picture's own units (BARRACKS-MARKER.md)
            const bool located = sc.zone == 1401 || sc.zone == 1501 || sc.zone == 1601 || barracks;
            if (barracks) {
                namespace m = xcx::map;
                const double want_x = m::BarracksOriginU + sc.x * m::BarracksScale - xcx::tiles::BarracksU0;
                const double want_y = xcx::tiles::BarracksV0 + xcx::tiles::BarracksH - (m::BarracksOriginV + sc.z * m::BarracksScale);
                Check(std::fabs(host.floats["xcx.pl_x"] - want_x) < 1e-3 && std::fabs(host.floats["xcx.pl_y"] - want_y) < 1e-3,
                      "Barracks marker is not in the picture's units");
            } else if (located) {
                Check(std::fabs(host.floats["xcx.pl_x"] - sc.x) < 1e-3 && std::fabs(host.floats["xcx.pl_y"] + sc.z) < 1e-3,
                      "Marker is not (x, -z)");
            } else {
                Check(std::isnan(host.floats["xcx.pl_x"]) && std::isnan(host.floats["xcx.pl_y"]), "Marker shown without a position");
            }
            if (located) { // the heading is read from actor+0x64 (contracts/PLAYER-HEADING.md)
                float raw{};
                auto *ab = host.Find(actor0 + 0x64, 4);
                Check(ab, "Actor yaw not in the sidecars");
                std::memcpy(&raw, ab->bytes.data() + actor0 + 0x64 - ab->address, 4);
                const bool finite = std::isfinite(raw) && std::fabs(raw) <= 4.f;
                Check(host.ints["xcx.player_yaw_valid"] == (finite ? 1 : 0), "Yaw validity");
                if (finite) {
                    const double expect = xcx::HeadingFromRaw(raw);
                    Check(std::fabs(host.floats["xcx.player_yaw"] - expect) < 1e-5, "Published yaw");
                    const int want = int(std::lround(std::fmod(expect * 180. / 3.14159265358979323846 + 360., 360.))) % 360;
                    Check(host.ints["xcx.player_yaw_deg"] == want, "Published yaw degrees");
                }
            } else {
                Check(host.ints["xcx.player_yaw_valid"] == 0 && host.ints["xcx.player_yaw_deg"] == 0, "No heading without a position");
            }
            // The page picture: the map hole is transparent where a widget draws, opaque for "not available".
            Pam page = request(host.frame, host.out / (std::string(sc.name) + "-page.pam"));
            Check(page.w == 1240 && page.h == 1080, "Page geometry");
            const bool hole = gate != 4;
            const auto alpha_at = [&](int x, int y) { return page.rgba[(std::size_t(y) * page.w + x) * 4 + 3]; };
            Check((alpha_at(430, 600) == 0) == hole, "The map hole is not transparent exactly when a widget shows");
            Check(alpha_at(1000, 500) == 255 && alpha_at(30, 1000) == 255, "The rail and tabs must stay opaque");
            // The pictures the visible overlays load (what the widgets ask for through their src_bind).
            Json out;
            out["name"] = sc.name;
            out["widget"] = names[gate];
            out["mode"] = sc.mode;
            out["zone"] = sc.zone;
            out["pl_x"] = std::isnan(host.floats["xcx.pl_x"]) ? Json(nullptr) : Json(host.floats["xcx.pl_x"]);
            out["pl_y"] = std::isnan(host.floats["xcx.pl_y"]) ? Json(nullptr) : Json(host.floats["xcx.pl_y"]);
            out["sk_kind"] = host.ints["xcx.sk_kind0"];
            out["yaw_deg"] = host.ints["xcx.player_yaw_deg"];
            out["view"] = {{"cx", sc.view_x}, {"cy", sc.view_y}, {"ppw", sc.view_ppw}, {"area", sc.view_area}};
            std::vector<std::string> shown;
            double best_opaque = 0;
            std::size_t hires_total_bytes = 0;
            const auto add = [&](const std::string &prefix, int count, const std::string &show_prefix) {
                for (int n = 0; n < count; ++n) {
                    const std::string key = prefix + std::to_string(n);
                    const std::string show = show_prefix + std::to_string(n);
                    if (host.ints[show]) {
                        const auto name = std::string(sc.name) + "-" + prefix.substr(4, 2) + std::to_string(n);
                        const Pam pam = request(host.texts[key], host.out / (name + ".pam"));
                        shown.push_back(prefix + std::to_string(n));
                        best_opaque = std::max(best_opaque, opaque_fraction(pam));
                        if (prefix == "xcx.mn." || prefix == "xcx.mh.")
                            hires_total_bytes += pam.rgba.size();
                    }
                }
            };
            add("xcx.ml.", 4, "xcx.mls.");
            add("xcx.mh.", 36, "xcx.mhs.");
            add("xcx.mn.", 16, "xcx.mns.");
            if (gate == 3)
                request(host.texts["xcx.mb"], host.out / (std::string(sc.name) + "-barracks.pam"));
            // An area map must have pictures that are not empty (an empty NLA map was seen live), and what
            // is shown at once must fit the runtime's 64 MiB picture budget with room to spare.
            if (gate == 1 || gate == 2)
                Check(best_opaque > 0.05, "The Area map has no picture with content");
            Check(hires_total_bytes <= 30u * 1024 * 1024, "Too many full-size pictures at once for the 64 MiB budget");
            out["overlays_shown"] = shown;
            std::ofstream(host.out / (std::string(sc.name) + ".json")) << out.dump(1);
            // Overlay gating is a function of the simulated view: expectations.
            const auto has = [&](const char *key) { return std::find(shown.begin(), shown.end(), key) != shown.end(); };
            std::cout << sc.name << ": widget " << names[gate] << ", overlays " << shown.size() << '\n';
            if (std::string(sc.name) == "primordia-area") {
                Check(has("xcx.ml.0") && has("xcx.mh.20") && !has("xcx.mh.0") && !has("xcx.mh.35"), "Hi tiles around the player");
            }
            if (std::string(sc.name) == "primordia-area-dragged-east") {
                Check(!has("xcx.mh.20") && has("xcx.mh.21"), "Dragging east must load the tile east of the player");
            }
            if (std::string(sc.name) == "primordia-area-pinched-out") {
                Check(shown.size() == 4 && !has("xcx.mh.20"), "A view wider than 4.5 km shows the quarter-resolution underlay only");
            }
            if (std::string(sc.name) == "primordia-area-pinched-mid")
                Check(shown.size() > 4 && shown.size() <= 10, "A 4 km view shows the underlay and at most six full-size pictures");
            if (std::string(sc.name) == "nla-area") {
                Check(shown.size() >= 1 && shown.size() <= 6, "NLA tiles around the player");
            }
            if (over)
                Check(shown.size() == 4, "Overworld shows the four quarter-resolution pictures");
            if (at_home)
                Check(std::isnan(host.floats["xcx.pl_x"]), "The home view has no marker");
        }
        // ---- Regression: a gesture burst on the Overworld while the game's files are slow ---------------------------
        // Live (HELPER-LIVE-14): after 20 s of drags and pinches on the Overworld only a strip of the map showed and the
        // chrome kept the old tab, because the host's single asset worker was busy building map pictures. Here a
        // stand-in asset worker serves the host's image requests in order (page picture, map pictures) while the
        // RomFS is slow and the drawn view of the Overworld widget is thrown about; the page picture must stay
        // prompt, the tab must follow the mode, and in the end every quarter of the world must be shown, complete.
        {
            struct Job {
                std::string key;
                std::chrono::steady_clock::time_point queued;
                double ms{-1};
                bool ok{};
                Pam pam;
            };
            std::mutex qm;
            std::condition_variable qcv;
            std::deque<std::shared_ptr<Job>> queue;
            bool stop = false;
            std::thread worker([&] {
                for (;;) {
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock lock(qm);
                        qcv.wait(lock, [&] { return stop || !queue.empty(); });
                        if (queue.empty())
                            return;
                        job = queue.front();
                        queue.pop_front();
                    }
                    Pam pam;
                    const bool ok = ext->load_image(module, &host.api, job->key.c_str(), &pam, sink);
                    std::scoped_lock lock(qm);
                    job->pam = std::move(pam);
                    job->ok = ok;
                    job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job->queued).count();
                }
            });
            // A failed Check below throws: stop and join the worker first, or the joinable thread's destructor
            // calls std::terminate and hides the Check message ("terminate called without an active exception").
            const auto stop_worker = [&] {
                {
                    std::unique_lock lock(qm);
                    stop = true;
                }
                qcv.notify_all();
                if (worker.joinable())
                    worker.join();
            };
            struct JoinGuard {
                const decltype(stop_worker) &f;
                ~JoinGuard() { f(); }
            } join_guard{stop_worker};
            const auto enqueue = [&](const std::string &key) {
                auto job = std::make_shared<Job>();
                job->key = key;
                job->queued = std::chrono::steady_clock::now();
                {
                    std::scoped_lock lock(qm);
                    queue.push_back(job);
                }
                qcv.notify_one();
                return job;
            };
            const auto tab_green = [](const Pam &pam, int n) { // mean green of the upper part of tab n (no text there)
                double sum = 0;
                int count = 0;
                for (int y = 84; y < 90; ++y)
                    for (int x = 16 + n * 204 + 60; x < 16 + n * 204 + 160; ++x) {
                        sum += pam.rgba[(std::size_t(y) * pam.w + x) * 4 + 1];
                        ++count;
                    }
                return sum / count;
            };
            // Armory Alley, Overworld tab, files slow, and a flag change so every world picture is rebuilt.
            host.Put<std::uint32_t>(zone_record + 0xac, 1401);
            host.Put<float>(actor0 + 0x50, -1470.28f);
            host.Put<float>(actor0 + 0x58, 7468.51f);
            host.flags["xcx.map_mode"] = 0;
            auto *rp = host.Find(host.api.main_base + 0x1dc1408, 8);
            Check(rp, "No saved-records root pointer");
            std::uint64_t root{};
            std::memcpy(&root, rp->bytes.data() + host.api.main_base + 0x1dc1408 - rp->address, 8);
            auto *fb = host.Find(root + xcx::FlagAreaOffset + 0x1ff0, 1);
            Check(fb, "The burst test needs XCX_FLAG_AREA (the saved flag area, e.g. build/xcx-mira/flags/progress.bin)");
            host.Put<std::uint8_t>(root + xcx::FlagAreaOffset + 0x1ff0, std::uint8_t(fb->bytes[root + xcx::FlagAreaOffset + 0x1ff0 - fb->address] ^ 0x80));
            // The scenario sweep's last page key is stale after the zone change above (the module rebuilds its frames and
            // the host just asks again for the new key), so let the module settle before the first request.
            for (int i = 0; i < 8; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                api->sample(module, &host.api);
            }
            // Cold reads of the game's files measure 13-33 ms each on the live NFS mount; 20 ms is the stand-in.
            host.read_delay_ms = std::getenv("XCX_BURST_READ_MS") ? std::atoi(std::getenv("XCX_BURST_READ_MS")) : 20;
            std::vector<std::shared_ptr<Job>> pages, maps, tab_checks;
            std::vector<int> tab_modes;
            std::string last_frame = host.frame, last_map_key[4];
            int mode = 0;
            const auto t0 = std::chrono::steady_clock::now();
            const auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
            unsigned step = 0;
            double next_toggle = 6.;
            // Throw the view about for 24 s (four directions, 500 ms each; three pinches), toggling the mode.
            while (seconds() < 24.) {
                const double ppw = 860. / 15000. * (step % 40 == 20 ? 0.5 : step % 40 == 25 ? 3. : 1.);
                const double dir[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                const auto &d = dir[(step / 4) % 4];
                host.view_f64["@map_view_cx:xcx_over"] = -1470. + d[0] * 3000. * ((step % 4) + 1);
                host.view_f64["@map_view_cy:xcx_over"] = -7468. + d[1] * 3000. * ((step % 4) + 1);
                host.view_f64["@map_view_ppw:xcx_over"] = ppw;
                host.view_f64["@map_view_w:xcx_over"] = 860;
                host.view_f64["@map_view_h:xcx_over"] = 960;
                if (seconds() >= next_toggle) {
                    mode ^= 1;
                    host.flags["xcx.map_mode"] = mode;
                    next_toggle += 5.;
                    // settle, then ask for the page picture exactly as the host does and check the tab it draws
                    for (int i = 0; i < 4; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        api->sample(module, &host.api);
                    }
                    tab_checks.push_back(enqueue(host.frame));
                    tab_modes.push_back(mode);
                    last_frame = host.frame;
                    pages.push_back(tab_checks.back());
                }
                api->sample(module, &host.api);
                if (host.frame != last_frame) {
                    last_frame = host.frame;
                    pages.push_back(enqueue(host.frame));
                }
                for (int n = 0; n < 4; ++n)
                    if (host.ints["xcx.mls." + std::to_string(n)] && host.texts["xcx.ml." + std::to_string(n)] != last_map_key[n]) {
                        last_map_key[n] = host.texts["xcx.ml." + std::to_string(n)];
                        maps.push_back(enqueue(last_map_key[n]));
                    }
                ++step;
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            // Everything the Overworld needs must end up shown and complete.
            const auto t1 = std::chrono::steady_clock::now();
            for (;;) {
                api->sample(module, &host.api);
                int shown = 0;
                for (int n = 0; n < 4; ++n)
                    shown += host.ints["xcx.mls." + std::to_string(n)];
                for (int n = 0; n < 4; ++n)
                    if (host.ints["xcx.mls." + std::to_string(n)] && host.texts["xcx.ml." + std::to_string(n)] != last_map_key[n]) {
                        last_map_key[n] = host.texts["xcx.ml." + std::to_string(n)];
                        maps.push_back(enqueue(last_map_key[n]));
                    }
                if (shown == 4)
                    break;
                Check(std::chrono::steady_clock::now() - t1 < std::chrono::seconds(300), "The world pictures never all became ready");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            host.read_delay_ms = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop_worker();
            double worst_page = 0;
            for (const auto &job : pages)
                worst_page = std::max(worst_page, job->ms);
            std::cout << "burst: page pictures " << pages.size() << " (slowest " << worst_page << " ms), map pictures served "
                      << maps.size() << ", elapsed " << seconds() << " s\n";
            const double slack = std::getenv("XCX_BURST_SLACK") ? std::atof(std::getenv("XCX_BURST_SLACK")) : 1.; // sanitizer builds are slower
            Check(worst_page >= 0 && worst_page < 2000. * slack, "A page picture waited behind map pictures (the chrome would go stale)");
            for (const auto &job : pages) {
                if (!job->ok)
                    std::cout << "  failed page request " << job->key << " (current frame " << host.frame << ")\n";
                Check(job->ok, "A page picture request failed");
            }
            for (std::size_t i = 0; i < tab_checks.size(); ++i) {
                const auto &pam = tab_checks[i]->pam;
                const int active = tab_modes[i], other = 1 - active;
                Check(pam.w == 1240 && tab_green(pam, active) > tab_green(pam, other) + 30., "The highlighted tab does not follow the mode");
            }
            Check(maps.size() >= 4, "The quarter-resolution pictures were never requested");
            for (const auto &job : maps) {
                Check(job->ok, "A shown world picture could not be served");
                Check(opaque_fraction(job->pam) > 0.95, "A shown world picture is incomplete");
            }
            // Stress invariant: whatever the module shows stays servable (a shown flag is never ahead of the cache).
            for (int n = 0; n < 4; ++n) {
                Check(host.ints["xcx.mls." + std::to_string(n)] != 0, "A shown world picture was withdrawn after the burst");
                Pam again;
                Check(ext->load_image(module, &host.api, host.texts["xcx.ml." + std::to_string(n)].c_str(), &again, sink) &&
                          opaque_fraction(again) > 0.95,
                      "A shown world picture is no longer served");
            }
            std::cout << "burst: every quarter of the world shown and complete; tab highlight followed " << tab_checks.size() << " mode switches\n";
        }
        // The arrow: the picture's tip points up, and rotating it by the published degrees points the tip
        // along the heading (sin h, -cos h) on the map, for the measured raw yaw values of PLAYER-HEADING.md.
        {
            const Pam arrow = request("module:xcx:map/arrow/0/0/0", host.out / "icon-arrow.pam");
            Check(arrow.w == 64 && arrow.h == 64, "Arrow picture size");
            double tx = 0, ty = 0, n = 0;
            int top = -1;
            for (int y = 0; y < 64 && top < 0; ++y)
                for (int x = 0; x < 64; ++x)
                    if (arrow.rgba[(std::size_t(y) * 64 + x) * 4 + 3] > 128)
                        top = y;
            for (int y = top; y < top + 3; ++y)
                for (int x = 0; x < 64; ++x)
                    if (arrow.rgba[(std::size_t(y) * 64 + x) * 4 + 3] > 128) {
                        tx += x;
                        ty += y;
                        ++n;
                    }
            const double ux = tx / n - 31.5, uy = ty / n - 31.5; // the tip from the picture centre (y down)
            Check(std::fabs(ux) < 1.5 && uy < -15, "The arrow picture does not point up");
            host.Put<std::uint32_t>(zone_record + 0xac, 1501);
            host.Put<float>(actor0 + 0x50, -2495.74f);
            host.Put<float>(actor0 + 0x58, 5748.33f);
            host.flags["xcx.map_mode"] = 1;
            const double raws[] = {-1.8326, 2.8798, 1.3090, -0.2618, -2.6180};
            const double walk[] = {-75, 15, 105, -165, -30};
            for (int k = 0; k < 5; ++k) {
                host.Put<float>(actor0 + 0x64, float(raws[k]));
                for (int i = 0; i < 4; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    api->sample(module, &host.api);
                }
                Check(host.ints["xcx.player_yaw_valid"] == 1, "Yaw not valid");
                const double deg = double(host.ints["xcx.player_yaw_deg"]);
                const double th = deg * 3.14159265358979323846 / 180;
                // the runtime turns the picture clockwise about its centre: screen vector (x, y down)
                const double rx = ux * std::cos(th) - uy * std::sin(th), ry = ux * std::sin(th) + uy * std::cos(th);
                const double screen = std::atan2(rx, -ry) * 180 / 3.14159265358979323846; // clockwise from up
                double diff = std::fmod(screen - walk[k] + 540., 360.) - 180.;
                Check(std::fabs(diff) < 2.5, "The rotated arrow does not point along the heading");
                std::cout << "  yaw raw " << raws[k] << " -> published " << deg << " deg, arrow tip " << screen << " deg (walk " << walk[k] << ")\n";
            }
            Check(host.ints["xcx.player_yaw_deg"] != 0, "Yaw never changed the published degrees");
            std::cout << "slowest picture " << slowest_picture_ms << " ms\n";
        }
        // The marker pictures the widgets draw (native icon_map cells), and the mode persisted in the flag.
        request(host.texts["xcx.msk"], host.out / "icon-skell.pam");
        request("module:xcx:map/arrow/0/0/0", host.out / "icon-arrow.pam");
        request(host.texts["xcx.mblank"], host.out / "blank.pam");
        Check(!ext->load_image(module, &host.api, "module:xcx:map/h/9/9/0", nullptr, [](void *, std::uint32_t, std::uint32_t, const std::uint8_t *, std::size_t) {}),
              "A bad map key was served");
        Check(!host.writes, "Map preview attempted a game write");
        // Exit while a world picture builds on slow files: a flag change makes every world picture stale, the
        // Area tab on Mira asks for them, and destroy waits for one source tile at most (about 12 reads, 0.5 s at
        // 40 ms a read), never for the rest of the picture in progress (16 source tiles, about 8 s).
        if (const char *fa = std::getenv("XCX_FLAG_AREA"); fa) {
            auto *rp = host.Find(host.api.main_base + 0x1dc1408, 8);
            std::uint64_t root{};
            if (rp)
                std::memcpy(&root, rp->bytes.data() + host.api.main_base + 0x1dc1408 - rp->address, 8);
            auto *fb = rp ? host.Find(root + xcx::FlagAreaOffset + 0x1ff0, 1) : nullptr;
            Check(fb, "The exit test needs XCX_FLAG_AREA");
            host.Put<std::uint8_t>(root + xcx::FlagAreaOffset + 0x1ff0,
                                   std::uint8_t(fb->bytes[root + xcx::FlagAreaOffset + 0x1ff0 - fb->address] ^ 0x40));
            host.Put<std::uint32_t>(zone_record + 0xac, 1501);
            host.Put<float>(actor0 + 0x50, 9000.f); // a corner of Mira no earlier step decoded (cold source tiles)
            host.Put<float>(actor0 + 0x58, -9000.f);
            host.flags["xcx.map_mode"] = 1;
            host.read_delay_ms = 40;
            for (int i = 0; i < 12; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                api->sample(module, &host.api);
            }
            const auto t0 = std::chrono::steady_clock::now();
            api->destroy(module);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            host.read_delay_ms = 0;
            std::cout << "exit while a world picture builds on slow files: destroy took " << ms << " ms\n";
            Check(ms < 1500., "Destroy waited for a world picture being built on slow files");
        } else {
            api->destroy(module);
        }
        dlclose(lib);
        std::cout << "Map widgets driven through the module ABI.\n";
        return 0;
    }
    if (motion_mode) {
        using Clock = std::chrono::steady_clock;
        std::vector<std::string> keys;
        // Sample like the host's tick for `ms`, collecting every new frame key; `active` records
        // whether the module ever asked for motion frames in that window.
        bool active = false;
        const auto pump = [&](int ms) {
            const auto end = Clock::now() + std::chrono::milliseconds(ms);
            while (Clock::now() < end) {
                api->sample(module, &host.api);
                active |= host.ints["xcx.motion_active"] == 1;
                if (keys.empty() || keys.back() != host.frame)
                    keys.push_back(host.frame);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
        };
        const auto draw = [&](const std::string &key, const std::string &name) {
            Output output{host.out / (name + ".ppm")};
            Check(ext->load_image(module, &host.api, key.c_str(), &output, Sink),
                  "Frame render failed");
        };
        const auto bytes_of = [&](const std::string &name) {
            std::ifstream f(host.out / (name + ".ppm"), std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        };
        pump(150); // the first samples read the party
        // 1. The Skell picture lands and fades in (Skell page); first, because the idle check below
        // needs the picture already there.
        Check(ext->on_action(module, "page", 2), "Page action refused");
        keys.clear();
        active = false;
        const auto started = Clock::now();
        while (host.ints["xcx.still_ready"] != 1) {
            pump(20);
            Check(Clock::now() - started < std::chrono::seconds(90), "Skell still never landed");
        }
        const std::size_t landed = keys.size();
        pump(900);
        Check(active && host.ints["xcx.motion_active"] == 0, "Picture fade never ran or never ended");
        const std::size_t first = landed > 1 ? landed - 1 : 0;
        Check(keys.size() - first >= 3 && keys.size() - first <= 8, "Fade frame count outside 3..8");
        std::cout << "still fade: " << keys.size() - first << " frames, motion_active ended\n";
        for (std::size_t i = first; i < keys.size(); ++i) {
            char n[32];
            std::snprintf(n, sizeof n, "still-fade-%02zu", i - first);
            draw(keys[i], n);
        }
        keys.clear();
        pump(1000);
        Check(keys.size() == 1, "Frames keep coming after the fade ended");
        // 2. Rest: the page points, the frame cadence, no motion requested.
        Check(ext->on_action(module, "page", 0), "Page action refused");
        pump(300);
        Check(host.ints["xcx.pg_fade"] == 0 && host.ints["xcx.pg_cut"] == -1 &&
                  host.ints["xcx.motion_reduced"] == 0 && host.ints["xcx.motion_active"] == 0,
              "Rest: page points / motion flags wrong");
        keys.clear();
        active = false;
        pump(1500); // idle: no new frame, no motion, for 1.5 s of samples
        Check(keys.size() == 1 && !active, "Idle: a frame was requested or motion was active at rest");
        std::cout << "idle: " << keys.size() << " frame key over 1500 ms, motion_active never set\n";
        draw(host.frame, "map");
        for (int p = 1; p <= 3; ++p) {
            Check(ext->on_action(module, "page", p), "Page action refused");
            pump(120);
            Check(host.ints["xcx.pg_fade"] == p && host.ints["xcx.pg_cut"] == -1,
                  "Page point does not follow the page");
            draw(host.frame, p == 1 ? "party" : p == 2 ? "skell" : "settings-motion-off");
        }
        // 3. Reduced motion (the runtime flag): points swap to the instant set, Settings shows ON.
        host.flags["motion.reduced"] = 1;
        pump(300);
        Check(host.ints["xcx.pg_fade"] == -1 && host.ints["xcx.pg_cut"] == 3 &&
                  host.ints["xcx.motion_reduced"] == 1,
              "Reduced motion: page points did not swap to the instant set");
        draw(host.frame, "settings-motion-on");
        Check(bytes_of("settings-motion-on") != bytes_of("settings-motion-off"),
              "Settings page does not show the Reduced motion state");
        // 4. HP roll on the Map rail: a big drop rolls over ~240 ms; reduced motion snaps.
        Check(ext->on_action(module, "page", 0), "Page action refused");
        host.flags["motion.reduced"] = 0;
        pump(400);
        keys.clear();
        active = false;
        host.Put<std::int32_t>(object + 0x250, 4000);
        pump(900);
        Check(active && host.ints["xcx.motion_active"] == 0, "HP roll never ran or never ended");
        Check(keys.size() >= 3 && keys.size() <= 8, "HP roll frame count outside 3..8");
        std::cout << "hp roll: " << keys.size() << " frames (including the one before), "
                  << "motion_active ended\n";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            char n[32];
            std::snprintf(n, sizeof n, "hp-roll-%02zu", i);
            draw(keys[i], n);
        }
        keys.clear();
        pump(1000);
        Check(keys.size() == 1, "Frames keep coming after the roll ended");
        draw(host.frame, "hp-after-roll");
        // The same drop with Reduced motion on lands in one step, byte-identical to the roll's end.
        host.Put<std::int32_t>(object + 0x250, 10000);
        pump(700);
        host.flags["motion.reduced"] = 1;
        pump(200);
        keys.clear();
        active = false;
        host.Put<std::int32_t>(object + 0x250, 4000);
        pump(700);
        Check(!active && keys.size() <= 2, "Reduced motion still rolled the HP");
        draw(host.frame, "hp-reduced");
        Check(bytes_of("hp-reduced") == bytes_of("hp-after-roll"),
              "Reduced-motion frame differs from the end of the roll");
        std::cout << "reduced motion: " << keys.size() << " frame(s) for the same drop\n";
        host.Put<std::int32_t>(object + 0x250, 10000);
        host.flags["motion.reduced"] = 0;
        pump(700);
        Check(!host.writes, "Motion preview attempted a game write");
        api->destroy(module);
        dlclose(lib);
        std::cout << "Motion preview passed through the module ABI.\n";
        return 0;
    }
    if (stills_mode) {
        // Skell pictures through the real module ABI: nothing until the job lands, then every
        // Skell-related page carries it. The first samples show the unchanged placeholder pages.
        const auto render_page = [&](const char *name, int page) {
            Check(ext->on_action(module, "page", page), "Page action refused");
            api->sample(module, &host.api);
            Output output{host.out / (std::string(name) + ".ppm")};
            Check(ext->load_image(module, &host.api, host.frame.c_str(), &output, Sink),
                  "Page render failed");
            return output;
        };
        api->sample(module, &host.api);
        render_page("skell-before-still", 2);
        const auto started = std::chrono::steady_clock::now();
        while (host.ints["xcx.still_ready"] != 1) {
            api->sample(module, &host.api);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(90),
                  "Skell still never landed");
        }
        const double ready_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
        std::cout << "still_ready_after_ms=" << ready_ms << '\n';
        render_page("skell-with-still", 2);
        render_page("map-with-still", 0);
        render_page("party-player-with-still", 1);
        Check(ext->on_action(module, "pick", 1), "Member selection refused");
        // The selected member's own Skell (a different frame) arrives later; show both states.
        render_page("party-elma-before-still", 1);
        for (int n = 0; n < 300; ++n) { // six seconds: long enough for the second frame's job
            api->sample(module, &host.api);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        render_page("party-elma-with-still", 1);
        // Standalone picture key.
        StandaloneImage standalone;
        const auto key = std::string("module:xcx:skell/") + std::to_string(220);
        const bool got = ext->load_image(module, &host.api, key.c_str(), &standalone,
                                         [](void *r, std::uint32_t w, std::uint32_t h,
                                            const std::uint8_t *p, std::size_t n) {
                                             auto *s = static_cast<StandaloneImage *>(r);
                                             s->w = int(w);
                                             s->h = int(h);
                                             s->rgba.assign(p, p + n);
                                         });
        Check(got && standalone.w == 700 && standalone.h == 760 &&
                  standalone.rgba.size() == 700u * 760 * 4,
              "Standalone Skell picture missing or wrong size");
        Check(!ext->load_image(module, &host.api, "module:xcx:skell/9999", &standalone,
                               [](void *, std::uint32_t, std::uint32_t, const std::uint8_t *,
                                  std::size_t) {}),
              "Standalone picture served for a frame that is not equipped");
        std::ofstream f(host.out / "skell-standalone.ppm", std::ios::binary);
        f << "P6\n700 760\n255\n";
        for (std::size_t i = 0; i < standalone.rgba.size(); i += 4) {
            const auto a = standalone.rgba[i + 3];
            for (int c = 0; c < 3; ++c) { // over the page's background colour for viewing
                const int bg = c == 0 ? 14 : c == 1 ? 19 : 33;
                const char v = char((standalone.rgba[i + c] * a + bg * (255 - a)) / 255);
                f.write(&v, 1);
            }
        }
        Check(!host.writes, "Still preview attempted a game write");
        api->destroy(module);
        dlclose(lib);
        std::cout << "Skell still pages rendered through the module ABI.\n";
        return 0;
    }
    Check(!ext->on_action(module, "board", 0) && !ext->on_action(module, "control", 1),
          "Unproven action accepted");
    // No helper was configured (no mailbox): the Skell button action is refused, never staged.
    Check(!ext->on_action(module, "skell_button", 0), "Skell button accepted without the helper");
    Json timings = Json::array();
    const auto render = [&](const char *name, int page) {
        Check(ext->on_action(module, "page", page), "Page action refused");
        api->sample(module, &host.api);
        Output output{host.out / (std::string(name) + ".ppm")};
        Check(ext->load_image(module, &host.api, host.frame.c_str(), &output, Sink),
              "Page render failed");
        const double total = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - output.requested)
                                 .count();
        timings.push_back({{"name", name},
                           {"prepare_ms", output.prepare_ms},
                           {"callback_with_file_sink_ms", total}});
        std::cout << output.path << " prepare_ms=" << output.prepare_ms << " total_ms=" << total
                  << '\n';
        return output;
    };
    const auto before = render("map", 0);
    render("map-warm", 0);
    Check(!ext->on_action(module, "follow", 1), "Unproved area-follow setting accepted");
    const auto actor =
        std::stoull(Json::parse(std::ifstream(fs::path(argv[3]) /
                                              "field-baseline/summary.json"))["actors"][0]["actor"]
                        .get<std::string>(),
                    nullptr, 16);
    host.Put<float>(actor + 0x50, s.party[0].pc.xyz[0] - 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    render("map-after-move", 0);
    // The marker is the native widget's now: it follows the published position every sample.
    Check(std::fabs(host.floats["xcx.pl_x"] - (s.party[0].pc.xyz[0] - 10)) < 1e-3,
          "Published marker did not follow the pilot");
    Check(before.marker_pixels == 0, "The page picture must not draw a marker over the native map");
    host.Put<float>(actor + 0x50, s.party[0].pc.xyz[0]);
    render("party-player", 1);
    Check(ext->on_action(module, "pick", 1), "Member selection refused");
    render("party-elma", 1);
    // The profile's Gear tab (PARTY-PROFILE-1): the page keeps the tabs, the list is its own picture.
    Check(!ext->on_action(module, "ptab", 2), "Unknown profile tab accepted");
    Check(ext->on_action(module, "ptab", 1), "Gear tab refused");
    render("party-elma-gear", 1);
    Check(host.ints["xcx.gear_tab"] == 1 && host.ints["xcx.gear_h"] > 700 && host.ints["xcx.gear_member"] == 11 &&
              host.texts["xcx.gear"].starts_with("module:xcx:gear/"),
          "Gear list not published");
    {
        Output gear{host.out / "party-elma-gearlist.ppm"};
        Check(ext->load_image(module, &host.api, host.texts["xcx.gear"].c_str(), &gear, GearSink),
              "Gear list render failed");
        Output stale{host.out / "must-not-exist.ppm"};
        Check(!ext->load_image(module, &host.api, "module:xcx:gear/0", &stale, GearSink), "Unknown gear key served");
    }
    Check(ext->on_action(module, "ptab", 0), "Overview tab refused");
    api->sample(module, &host.api);
    Check(host.ints["xcx.gear_tab"] == 0 && host.texts["xcx.gear"].empty(), "Gear list left on the Overview tab");
    // Overview and Gear for every member (the visual check against the native Status pages), then back to Elma.
    for (const auto &[slot, name] : std::array<std::pair<int, const char *>, 4>{
             {{0, "player"}, {1, "elma"}, {2, "lin"}, {3, "irina"}}}) {
        Check(ext->on_action(module, "pick", slot), "Member selection refused");
        render(("profile-" + std::string(name) + "-overview").c_str(), 1);
        Check(ext->on_action(module, "ptab", 1), "Gear tab refused");
        render(("profile-" + std::string(name) + "-gear").c_str(), 1);
        Output gear{host.out / ("profile-" + std::string(name) + "-gearlist.ppm")};
        Check(ext->load_image(module, &host.api, host.texts["xcx.gear"].c_str(), &gear, GearSink),
              "Gear list render failed");
        Check(ext->on_action(module, "ptab", 0), "Overview tab refused");
    }
    Check(ext->on_action(module, "pick", 1), "Member selection refused");
    api->sample(module, &host.api);
    render("skell", 2);
    Check(!ext->on_action(module, "ptab", 1), "Profile tab accepted off the Party page");
    render("settings", 3);
    const auto old_key = host.frame;
    Output rejected{host.out / "must-not-exist.ppm"};
    auto wrong = host.api;
    wrong.title_id ^= 1;
    const auto reads = host.reads; const int rom_reads = host.rom_reads;
    Check(!ext->load_image(module, &wrong, old_key.c_str(), &rejected, Sink),
          "Wrong-title image accepted");
    api->sample(module, &wrong);
    Check(host.reads == reads && host.rom_reads == rom_reads,
          "Wrong title caused a memory/asset read");
    wrong = host.api;
    wrong.build_id[31] ^= 1;
    Check(!ext->load_image(module, &wrong, old_key.c_str(), &rejected, Sink),
          "Wrong-build image accepted");
    api->sample(module, &wrong);
    Check(host.reads == reads && host.rom_reads == rom_reads,
          "Wrong build caused a memory/asset read");
    Check(!ext->load_image(module, &host.api, old_key.c_str(), &rejected, Sink),
          "Revoked picture survived invalid host");
    wrong = host.api;
    wrong.abi_hash ^= 1;
    api->sample(module, &wrong);
    Check(!ext->load_image(module, &wrong, old_key.c_str(), &rejected, Sink),
          "Wrong-ABI image accepted");
    render("recovered", 0);
    const auto valid_key = host.frame;
    host.read_throws = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    api->sample(module, &host.api);
    Check(!ext->on_action(module, "pick", 1),
          "Old party selection accepted after acquisition failure");
    Check(!ext->load_image(module, &host.api, valid_key.c_str(), &rejected, Sink),
          "Old image survived acquisition failure");
    host.read_throws = false;
    Output waiting{host.out / "waiting.ppm"};
    Check(ext->load_image(module, &host.api, host.frame.c_str(), &waiting, Sink),
          "Unavailable frame did not render");
    Check(!host.writes, "Read-only preview attempted a game write");
    // ---- Guest helper through the real module ABI: a fake mailbox and a fake guest -------------
    // Not game memory: the helper's mailbox is its own word array, so zero game writes still hold.
    struct FakeMailbox {
        std::array<std::uint32_t, 1024> w{};
    } mailbox;
    EdenDsmodHostExtensions hx{};
    hx.version = EDEN_DSMOD_EXT_VERSION;
    hx.struct_size = sizeof hx;
    hx.abi_hash = EDEN_DSMOD_EXT_HASH;
    hx.userdata = &mailbox;
    hx.mailbox_address = 0x7000;
    hx.mailbox_size = 4096;
    hx.load_u32 = [](void *u, std::uint32_t off, std::uint32_t *v) -> EdenDsmodBool {
        if (off % 4 || off >= 4096)
            return false;
        *v = static_cast<FakeMailbox *>(u)->w[off / 4];
        return true;
    };
    hx.store_u32 = [](void *u, std::uint32_t off, std::uint32_t v) -> EdenDsmodBool {
        if (off % 4 || off >= 4096)
            return false;
        static_cast<FakeMailbox *>(u)->w[off / 4] = v;
        return true;
    };
    namespace xh = xcx_helper;
    const auto word = [&](std::uint32_t off) -> std::uint32_t & { return mailbox.w[off / 4]; };
    mailbox.w[xh::EpochOffset / 4] = 5;
    ext->configure(module, &hx);
    Check(word(xh::OffsetModuleMagic) == xh::ModuleMagic, "Helper mailbox not claimed");
    std::uint32_t state = xh::StateValid | xh::StateOnFoot | xh::StateGate | xh::StateHudOk | xh::StateCanBoard;
    bool guest_alive = true, guest_answers = false, input_ready = true;
    std::uint32_t answer_code = 0;
    int calls = 0;
    // One game frame of the fake guest, then one module sample.
    const auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            if (guest_alive) {
                word(xh::OffsetGuestMagic) = xh::GuestMagic;
                ++word(xh::OffsetHeartbeat);
                if (++calls % 5 == 0) {
                    word(xh::OffsetState) = state | (input_ready ? xh::StateInputReady : 0) | xh::StateFieldOk;
                    ++word(xh::OffsetPollBeat);
                }
                const auto seq = word(xh::OffsetReqSeq);
                if (guest_answers && seq != word(xh::OffsetAckSeq)) {
                    word(xh::OffsetResultCode) = answer_code;
                    const bool is_switch = word(xh::OffsetReqOp) == 3;
                    word(xh::OffsetResultValue) = is_switch ? word(xh::OffsetReqTarget) : answer_code == 0;
                    if (is_switch && answer_code == 0)
                        word(xh::OffsetSlot0Token) = word(xh::OffsetReqTarget); // the party reordered
                    word(xh::OffsetAckSeq) = seq;
                }
            }
            api->sample(module, &host.api);
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
    };
    const auto bytes = [&](const char *name) {
        std::ifstream f(host.out / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    };
    const auto skell_baseline = bytes("skell.ppm");
    frames(60);
    Check(host.ints["xcx.helper"] == 1, "Fresh helper not published");
    render("skell-board", 2);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    render("map-board", 0);
    frames(40);
    Check(bytes("skell-board.ppm") != skell_baseline, "Board face did not replace the prototype's");
    Check(ext->on_action(module, "skell_button", 0), "Board tap refused while eligible");
    Check(word(xh::OffsetReqSeq) != word(xh::OffsetAckSeq) && word(xh::OffsetReqOp) == 1 &&
              word(xh::OffsetReqEpoch) == 5,
          "Board request not staged");
    Check(!ext->on_action(module, "skell_button", 0), "Second tap accepted while a request is in flight");
    guest_answers = true;
    frames(4);
    Check(word(xh::OffsetReqSeq) == word(xh::OffsetAckSeq), "Fake guest did not answer");
    guest_answers = false;
    render("skell-working", 2);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    Check(!ext->on_action(module, "skell_button", 0), "Tap accepted while waiting for the flip");
    state = xh::StateValid | xh::StatePiloting | xh::StateGate | xh::StateHudOk | xh::StateCanExit;
    frames(14);
    render("skell-exit-lockout", 2);
    Check(!ext->on_action(module, "skell_button", 0), "Tap accepted inside the 600 ms lockout");
    frames(70);
    render("skell-exit", 2);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    render("map-exit", 0);
    frames(40);
    Check(ext->on_action(module, "skell_button", 0) && word(xh::OffsetReqOp) == 2, "Exit tap not staged");
    answer_code = 7; // the game refuses: a refusal reason, then usable again
    guest_answers = true;
    frames(4);
    guest_answers = false;
    render("map-exit-refused", 0);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    Check(host.ints["xcx.dbg.last_result"] == 7 && host.ints["xcx.dbg.heartbeat"] > 0 &&
              host.ints["xcx.dbg.answered"] == 0,
          "Live-test diagnostics not published");
    // Controlled-character switch from the Party page's Set plate.
    answer_code = 0;
    state = xh::StateValid | xh::StateOnFoot | xh::StateGate | xh::StateHudOk | xh::StateIdle |
            xh::StateCooldownClear | xh::StateSwitchReady;
    frames(80);
    Check(ext->on_action(module, "pick", 1), "Party pick refused");
    frames(6);
    render("party-set", 1);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    Check(bytes("party-set.ppm") != bytes("party-elma.ppm"), "Set plate did not light up");
    Check(ext->on_action(module, "switch_set", 0), "Set tap refused while switch_ready");
    Check(word(xh::OffsetReqOp) == 3 && word(xh::OffsetReqArg) == 1 &&
              word(xh::OffsetReqTarget) == xh::MemberToken(11),
          "Switch request not staged with slot and target member");
    Check(!ext->on_action(module, "switch_set", 0), "Second Set tap accepted in flight");
    guest_answers = true;
    frames(4);
    guest_answers = false;
    Check(!ext->on_action(module, "switch_set", 0), "Set tap accepted right after a submitted switch");
    state &= ~xh::StateSwitchReady; // cooldown
    frames(60);
    Check(!ext->on_action(module, "switch_set", 0), "Set tap accepted during the cooldown");
    Check(ext->on_action(module, "pick", 0) && !ext->on_action(module, "switch_set", 0),
          "Controlled member's row switched");
    // The `+` menu: the HUD update still runs but the game does not accept input. Every action is
    // refused and the page is the prototype's.
    state = xh::StateValid | xh::StateOnFoot | xh::StateGate | xh::StateHudOk | xh::StateIdle |
            xh::StateCooldownClear | xh::StateSwitchReady | xh::StateCanBoard;
    frames(80);
    Check(ext->on_action(module, "pick", 1), "Party pick refused");
    frames(6);
    Check(ext->on_action(module, "switch_set", 0), "Set tap refused while the field accepts input");
    guest_answers = true;
    frames(4);
    guest_answers = false;
    frames(120);
    input_ready = false;
    frames(60);
    Check(host.ints["xcx.helper"] == 1 && host.ints["xcx.set_enabled"] == 0, "Set plate stayed enabled in the menu");
    Check(!ext->on_action(module, "switch_set", 0), "Set tap accepted in the menu");
    Check(ext->on_action(module, "page", 2) && !ext->on_action(module, "skell_button", 0),
          "Skell button tap accepted in the menu");
    render("skell-menu", 2);
    frames(40); // the page render stalls the fake guest: let the helper be seen again
    // Since 2026-11-25 (contracts/STATES-1.md) a native menu dims the page with a "Menu open" plate.
    Check(host.ints["xcx.menu"] == 1 && bytes("skell-menu.ppm") != skell_baseline,
          "Menu did not dim the Skell page");
    input_ready = true;
    frames(60);
    // Return to Skell: far from the Skell on foot with can_return, a 600 ms hold acts, a tap only hints.
    answer_code = 0;
    state = xh::StateValid | xh::StateOnFoot | xh::StateGate | xh::StateHudOk | xh::StateIdle |
            xh::StateCooldownClear | xh::StateSwitchReady | xh::StateCanReturn | xh::StateFieldOk;
    frames(140);
    Check(ext->on_action(module, "page", 2), "Skell page refused");
    frames(10);
    Check(host.ints["xcx.face_kind"] == 3 && host.ints["xcx.return_ready"] == 1, "Return face not offered");
    render("skell-return", 2);
    render("map-return", 0);
    frames(40);
    Check(ext->on_action(module, "page", 2) && host.ints["xcx.return_ready"] == 1, "Return face lost");
    Check(!ext->on_action(module, "skell_button", 0), "A short tap acted on the Return face");
    frames(4);
    Check(host.ints["xcx.return_hint"] == 1, "No Hold hint after a short tap");
    render("skell-return-hint", 2);
    render("map-return-hint", 0);
    frames(40);
    Check(ext->on_action(module, "page", 2), "Skell page refused");
    Check(ext->on_action(module, "return_hold", 0) && word(xh::OffsetReqOp) == 4, "Return hold not staged");
    Check(!ext->on_action(module, "return_hold", 0), "Second Return hold accepted in flight");
    guest_answers = true;
    frames(4);
    guest_answers = false;
    Check(host.ints["xcx.dbg.last_result"] == 0, "Return not answered");
    frames(40);
    Check(host.ints["xcx.return_ready"] == 0 && !ext->on_action(module, "return_hold", 0),
          "Return offered again while the warp loads");
    Check(host.ints["xcx.dbg.can_return"] == 1 && host.ints["xcx.dbg.field_ok"] == 1, "Return diagnostics missing");
    state = xh::StateValid | xh::StatePiloting | xh::StateGate | xh::StateHudOk | xh::StateIdle |
            xh::StateCooldownClear | xh::StateCanExit; // the Skell is driven: the wait ends
    frames(80);
    // The helper goes silent (menu, event, loading): the page is the prototype's again, tap refused.
    guest_alive = false;
    frames(60);
    Check(host.ints["xcx.helper"] == 0, "Silent helper still published as present");
    render("skell-stale", 2);
    Check(bytes("skell-stale.ppm") == skell_baseline, "Stale helper changed the prototype page");
    Check(!ext->on_action(module, "skell_button", 0), "Tap accepted with a stale helper");
    Check(!host.writes, "Helper preview attempted a game write");
    ext->configure(module, nullptr); // helper withdrawn
    api->sample(module, &host.api);
    Check(word(xh::OffsetModuleMagic) == 0, "Helper magic not withdrawn on reconfigure");
    std::ofstream(host.out / "timings.json")
        << Json{{"scope", "cold module instance / warm OS file cache; preparation excludes test "
                          "PPM disk sink; not live runtime/Thor timing"},
                {"renders", timings}}
               .dump(2)
        << '\n';
    api->destroy(module);
    dlclose(lib);
    std::cout << "Build refusal, snapshot rejection, read-only actions and actual ABI page renders "
                 "passed.\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
