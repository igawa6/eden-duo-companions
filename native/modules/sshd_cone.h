// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD: Link's view cone on the maps keeps the last picture that is known to be loaded
// (owner Thor feedback 2026-11-12: "bottom view cone some time blinking when move").
//
// The cone is a map dynamic marker whose picture is one of 72 pre-turned module images
// (module:sshd:cone/<deg>, 5-degree steps, 400 px squares). The host draws nothing for a marker
// whose picture is still loading (runtime 14: "one still loading draws nothing yet"), and its
// module-image cache is an LRU over 64 MB shared with the map pictures, so a turn onto an angle
// that was never built, or was evicted by a map picture, blanked the cone until the asset worker
// built it again: a frame or two on the desktop, much longer on a handheld whose worker is slower
// and often busy with a map picture.
//
// ConeHold keeps showing the last angle whose picture the module has delivered and which is still
// likely cached, and asks the host for the wanted angle through a second, off-screen marker (the
// prefetch: the host loads a picture when a marker names it, wherever the marker is). Once the
// module has delivered the wanted picture it is shown. "Likely cached": delivered, not being built
// again, and fewer than kStaleBytes of other module images delivered since it was last delivered
// or named by a marker (the host evicts the least recently drawn picture when the 64 MB fill up).
// A picture the host still holds is never requested again, so a stale-looking angle that has been
// wanted for kWait samples without a request is shown anyway. While the wanted angle loads, a
// nearer loaded angle is shown when there is one. A failed read keeps the last answer for kMiss
// samples (an off-thread read can land mid-update), then hides the cone.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace sshd {

class ConeHold {
public:
    static constexpr int kSteps = 72;                          // cone/<5 * i>
    static constexpr uint64_t kStaleBytes = 16ull << 20;       // of the host's 64 MB image cache
    static constexpr int kWait = 45;                           // samples
    static constexpr int kMiss = 30;                           // samples

    struct Out {
        bool on = false;
        int shown = -1;    // degrees of the picture to draw (-1 none yet)
        int prefetch = -1; // degrees the off-screen marker asks for (-1 none)
    };

    // --- the module's asset worker (load_image)
    void Building(int deg) {
        std::lock_guard l(mu_);
        if (const int i = Index(deg); i >= 0)
            building_[i] = true;
    }
    void Delivered(int deg, bool ok) {
        std::lock_guard l(mu_);
        if (const int i = Index(deg); i >= 0) {
            building_[i] = false;
            if (ok) {
                delivered_[i] = true;
                delivered_at_[i] = total_;
            }
        }
    }
    /// Every module image delivered to the host (any key), in bytes (w * h * 4).
    void Bytes(uint64_t n) {
        std::lock_guard l(mu_);
        total_ += n;
    }

    // --- the sample: `read_ok` = the stage / facing reads succeeded; `on` = the cone belongs on
    // the map; `deg` = the wanted angle (a multiple of 5 in 0..355). `legacy` replays dev.26..36
    // (the wanted angle at once) for diagnostics.
    Out Step(bool read_ok, bool on, int deg, bool legacy = false) {
        std::lock_guard l(mu_);
        if (!read_ok) {
            if (on_ && miss_ < kMiss) {
                ++miss_;
                return {true, Deg(shown_), -1};
            }
            on_ = false;
            return {false, Deg(shown_), -1};
        }
        miss_ = 0;
        on_ = on;
        const int want = Index(deg);
        if (!on || want < 0)
            return {false, Deg(shown_), -1};
        if (legacy || shown_ < 0) {
            shown_ = want; // nothing loaded to hold yet (or the old behaviour)
        } else if (want != shown_) {
            if (want_ != want) {
                want_ = want;
                wait_ = 0;
            }
            ++wait_;
            if (Fresh(want) || (wait_ > kWait && !building_[want])) {
                shown_ = want;
            } else {
                // while it loads: the loaded angle nearest to the wanted one, if nearer than now
                int best = shown_, best_d = Dist(shown_, want);
                for (int i = 0; i < kSteps; ++i)
                    if (Dist(i, want) < best_d && Fresh(i)) {
                        best = i;
                        best_d = Dist(i, want);
                    }
                shown_ = best;
            }
        }
        const int pf = !legacy && want != shown_ ? want : -1;
        // the host touches what the markers name each time it draws them; a stale prefetch may have
        // been evicted (then it is requested again), so naming it does not make it fresh
        used_at_[shown_] = total_;
        if (pf >= 0 && Fresh(pf))
            used_at_[pf] = total_;
        return {true, Deg(shown_), Deg(pf)};
    }

    /// The shown picture has been delivered and is not being built again (diagnostics: a cone the
    /// host can draw; false = a blank cone).
    bool ShownReady() const {
        std::lock_guard l(mu_);
        return shown_ >= 0 && delivered_[shown_] && !building_[shown_];
    }
    void Reset() {
        std::lock_guard l(mu_);
        shown_ = want_ = -1;
        wait_ = miss_ = 0;
        on_ = false;
    }

private:
    static int Index(int deg) { return deg >= 0 && deg < 360 && deg % 5 == 0 ? deg / 5 : -1; }
    static int Deg(int i) { return i < 0 ? -1 : i * 5; }
    static int Dist(int a, int b) {
        const int d = std::abs(a - b) % kSteps;
        return d > kSteps / 2 ? kSteps - d : d;
    }
    bool Fresh(int i) const {
        const uint64_t seen = delivered_at_[i] > used_at_[i] ? delivered_at_[i] : used_at_[i];
        return delivered_[i] && !building_[i] && total_ - seen < kStaleBytes;
    }

    mutable std::mutex mu_;
    uint64_t total_ = 0;
    uint64_t delivered_at_[kSteps]{}, used_at_[kSteps]{};
    bool delivered_[kSteps]{}, building_[kSteps]{};
    int shown_ = -1, want_ = -1, wait_ = 0, miss_ = 0;
    bool on_ = false;
};

} // namespace sshd
