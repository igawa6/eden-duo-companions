// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH companion diagnostics (acnh_publish.h BuildDiag): what each live domain resolved to, on the
// device. Lines acnh.diag.0 .. acnh.diag.<n-1> for the hidden overlay on the NookPhone page (5 taps
// on the phone clock within 3 s opens it, a tap on it closes it: ui.diag), and the same lines as
// one "ACNH diag:" log line whenever they change (at most every 5 s), so a pulled Eden log carries
// them even without a screenshot. Read-only: nothing here touches the game.

#include "acnh_config.h"
#include "acnh_publish.h"
#include "acnh_publish_detail.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace acnh {
#if EDEN_ACNH_DIAGNOSTICS
using namespace pub;

namespace {
std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* f, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
const char* Yn(bool b) {
    return b ? "ok" : "NO";
}
} // namespace

bool Publisher::DiagAction(std::string_view action) {
    const uint64_t now = live::NowMs();
    if (action == "diag_off") {
        diag_on = false;
        diag_taps = 0;
        return true;
    }
    // diag_tap: the 5th tap within 3 s of the first toggles the overlay
    if (!diag_taps || now - diag_tap_ms > 3000) {
        diag_taps = 0;
        diag_tap_ms = now;
    }
    if (++diag_taps >= 5) {
        diag_on = !diag_on;
        diag_taps = 0;
    }
    return true;
}

void Publisher::BuildDiag(const EdenDsmodHostApi& host, const live::LiveSnapshot& s,
                          int64_t sample_us) {
    std::vector<std::string> l;
    const uint64_t heap_mib = s.heap_hi > s.heap_lo ? (s.heap_hi - s.heap_lo) >> 20 : 0;
    l.push_back(Fmt("main 0x%" PRIx64 " +0x%" PRIx64 "  reloc %" PRId64 "  heap 0x%" PRIx64
                    "..0x%" PRIx64 " (%" PRIu64 " MiB)",
                    s.main_base, s.main_size, s.reloc, s.heap_lo, s.heap_hi, heap_mib));
    const bool in_heap =
        s.main_dat && s.heap_hi > s.heap_lo && s.main_dat >= s.heap_lo && s.main_dat < s.heap_hi;
    l.push_back(Fmt("main.dat 0x%" PRIx64 " %s", s.main_dat,
                    !s.main_dat ? "(none)"
                    : in_heap   ? "in heap"
                                : "OUTSIDE heap range"));
    l.push_back(s.dead_pins.empty()
                    ? std::string{"pins: all match"}
                    : Fmt("pins dead (domains 0x%x): %s", s.dead_domains, s.dead_pins.c_str()));
    l.push_back(Fmt("save %s  personal %s  clock %s  lang %s %s  pos %s  player %d", Yn(s.save_ok),
                    Yn(s.personal_ok), Yn(s.clock_ok), Yn(s.lang_ok), s.lang_folder.c_str(),
                    Yn(s.pos_ok), s.player_no));
    l.push_back(Fmt("scene: phone %s %s  player %s %s  camera %s %s", Yn(s.phone_ok),
                    s.phone.name.c_str(), Yn(s.player_ok), s.player.name.c_str(), Yn(s.camera_ok),
                    s.camera.name.c_str()));
    l.push_back(Fmt("apps: defs %zu  flags %s  grid %s (n %d cursor %d)  shown %zu (page %d)",
                    s.app_defs.size(), Yn(s.flags_ok), Yn(s.phone_ui.ok), s.phone_ui.count,
                    s.phone_ui.cursor, apps.size(), page));
    l.push_back("err: " + (s.diag.empty() ? std::string{"-"} : s.diag));
    // the log line: once per change of everything above but the scene states (they move with
    // every step the player takes), at most every 5 s
    uint64_t key = 0xd1a9;
    for (size_t i = 0; i < l.size(); ++i)
        if (i != 4)
            key = MixBytes(key, l[i].data(), l[i].size());
    key = Mix(key, (uint64_t(s.phone_ok) << 2) | (uint64_t(s.player_ok) << 1) | s.camera_ok);
    const uint64_t now = live::NowMs();
    if (key != diag_log_key && now - diag_log_ms >= 5000 && host.log) {
        diag_log_key = key;
        diag_log_ms = now;
        std::string line = "ACNH diag:";
        for (const auto& x : l)
            line += " | " + x;
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
    }
    l.push_back(Fmt("sample %" PRId64 " us  #%" PRIu64, sample_us, s.serial));
    if (!diag_on) { // the overlay's gate, every sample like every other published value
        if (host.publish_i64)
            host.publish_i64(host.userdata, "ui.diag", 0);
        return;
    }
    Out o;
    o.I("ui.diag", 1);
    o.I("acnh.diag.n", static_cast<int64_t>(l.size()));
    for (size_t i = 0; i < l.size(); ++i)
        o.T("acnh.diag." + std::to_string(i), l[i]);
    Emit(host, o);
}

#else
bool Publisher::DiagAction(std::string_view) {
    return false;
}
void Publisher::BuildDiag(const EdenDsmodHostApi&, const live::LiveSnapshot&, int64_t) {}
#endif
} // namespace acnh
