// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Now-playing BGM (p5r_bgm_reader.h): SampleBgm updates the tracker once per sample and logs cue
// changes; PublishBgm publishes bgm.*.

#include "p5r_reader.h"

namespace p5r_module {

// Now-playing BGM: a few bounded words per sample (p5r_bgm_reader.h).
void Reader::SampleBgm(bool ready) {
    bgm_frame =
        bgm_resolved
            ? bgm.Update([&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); },
                         bgm_roots, ready, text)
            : p5r_bgm::Frame{};
    if ((bgm_frame.cue != bgm_logged_cue || bgm_frame.change != bgm_logged_change) && host.log) {
        char message[160];
        std::snprintf(message, sizeof(message), "P5R bgm: cue=%lld change=%lld row=%lld [%s]",
                      static_cast<long long>(bgm_frame.cue),
                      static_cast<long long>(bgm_frame.change),
                      static_cast<long long>(bgm_frame.index), bgm_frame.title);
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, message);
    }
    bgm_logged_cue = bgm_frame.cue;
    bgm_logged_change = bgm_frame.change;
}
// bgm.* outputs: ready, playing, cue, title, known, index, change; bgm.fade: -1 fading out,
// 1 fading in.
void Reader::PublishBgm() const {
    const auto& b = bgm_frame;
    I64("bgm.ready", b.ready);
    I64("bgm.playing", b.playing);
    I64("bgm.cue", b.cue);
    Text("bgm.title", b.title);
    I64("bgm.known", b.known);
    I64("bgm.index", b.index);
    I64("bgm.change", b.change);
    I64("bgm.fade", b.fade);
}

} // namespace p5r_module
