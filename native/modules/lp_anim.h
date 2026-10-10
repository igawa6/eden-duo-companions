// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Procedural sprite motion for the Luminescent Platinum companion: pure functions of a monotonic
// time in ms, plus two small trackers that turn what the module reads (a battler's shown HP and
// status, the selected Pokémon) into a pose. The module publishes the poses as integers; the
// manifest gates them with the "Animations" setting (a derived value per channel) and binds them
// to the sprites' x_bind / y_bind / rotate_bind / scale_bind / tint_bind.
//   - hit: a decaying shake when the top screen's HP gauge starts to drain;
//   - hop: one arc up and back when a Pokémon is selected;
//   - breath: a slow scale pulse of the focused sprite, pivot at its feet;
//   - faint: sink and fade once the shown HP reaches 0, then stay there;
//   - status loops: asleep bobs, paralysed twitches now and then, red HP wobbles.
// Battle priority: faint > hit > status. Every motion ends exactly at its rest value.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lp_anim {

struct Pose {
    int x = 0, y = 0; // px offset (y down)
    int rot = 0;      // degrees, clockwise, about the widget's pivot
    int scale = 1000; // x1000, about the widget's pivot
    int fade = 0;     // tint index: 0 opaque .. FadeSteps faintest
    bool Rest() const {
        return x == 0 && y == 0 && rot == 0 && scale == 1000 && fade == 0;
    }
    bool operator==(const Pose&) const = default;
};

// ---- design values (ms, px) ----
constexpr double HitMs = 400;      // shake length from the drain's start
constexpr double HitQuietMs = 250; // a drain that resumes after this gap is a new hit
constexpr double HitStepMs = 40;   // a new offset every 40 ms (25 Hz)
constexpr int HitPxX = 6, HitPxY = 2;
constexpr double HopMs = 300;
constexpr int HopPx = 14;
constexpr double BreathMs = 2400;
constexpr int BreathMilli = 25; // 1.000 .. 1.025
constexpr int BreathStep = 5;   // published in 0.5 % steps (about 1 px on the 210 px card sprite)
constexpr double FaintMs = 600;
constexpr int FaintPx = 20, FaintPxCard = 14; // the header sprite / the double card's smaller one
constexpr int FadeSteps = 8;                  // tint_colors has FadeSteps + 1 entries
constexpr double SleepMs = 1600;
constexpr int SleepPx = 4;
constexpr double TwitchEveryMs = 2500, TwitchMs = 150, TwitchStepMs = 30;
constexpr int TwitchPx = 3;
constexpr double WobbleMs = 1200;
constexpr int WobbleDeg = 2;

constexpr double Never = -1e18;
inline constexpr double Pi = 3.14159265358979323846;

inline int Round(double v) {
    return static_cast<int>(std::lround(v));
}
// 0 at t = 0, 1 at half the period, 0 again at the period (a cosine bump, no jump at the start)
inline double Bump(double t, double period) {
    return (1.0 - std::cos(2.0 * Pi * std::fmod(t, period) / period)) * 0.5;
}

// One hop: a ballistic arc (ease-out up, ease-in down), -HopPx at the middle; 0 outside the hop.
inline int HopY(double t) {
    if (!(t >= 0.0 && t < HopMs))
        return 0;
    const double u = t / HopMs;
    return -Round(HopPx * 4.0 * u * (1.0 - u));
}

// The native roster has already exchanged slots. Start each new card at its old
// position, slide right, exchange rows, then return to the column. Other slots rest.
inline constexpr double PartySwapMs = 560;
inline Pose PartySwap(double elapsed, int slot, int from, int to) {
    Pose pose;
    if (!(elapsed >= 0 && elapsed < PartySwapMs) || from < 0 || to < 0 || from == to ||
        (slot != from && slot != to)) return pose;
    const double travel = std::clamp((elapsed - 120.0) / 320.0, 0.0, 1.0);
    const double eased = travel * travel * (3.0 - 2.0 * travel);
    const double slide = std::min({elapsed / 120.0, (PartySwapMs - elapsed) / 120.0, 1.0});
    pose.x = Round((slot == to ? 28 : 48) * slide);
    pose.y = Round((slot == to ? from - to : to - from) * 150 * (1.0 - eased));
    return pose;
}

// The hit shake: alternating offsets every HitStepMs, decaying linearly to 0 at HitMs.
inline Pose HitShake(double t) {
    Pose p;
    if (!(t >= 0.0 && t < HitMs))
        return p;
    static constexpr int Ys[] = {1, -1, 0, 1, -1, 0, 1, -1, 0, 1};
    const int step = static_cast<int>(t / HitStepMs);
    const double decay = 1.0 - t / HitMs;
    p.x = (step % 2 == 0 ? 1 : -1) * Round(HitPxX * decay);
    p.y = Ys[step % 10] * Round(HitPxY * decay);
    return p;
}

// Faint: sinks `px` and fades to FadeSteps over FaintMs (ease-in), then stays there.
inline Pose Faint(double t, int px) {
    const double u = t <= 0.0 ? 0.0 : t >= FaintMs ? 1.0 : t / FaintMs;
    const double e = u * u;
    Pose p;
    p.y = Round(px * e);
    p.fade = Round(FadeSteps * e);
    return p;
}

// Breathing: 1000 .. 1000 + BreathMilli and back every BreathMs, in BreathStep steps; 1000 before 0.
inline int Breath(double t) {
    if (!(t >= 0.0))
        return 1000;
    return 1000 + BreathStep * Round(Bump(t, BreathMs) * BreathMilli / BreathStep);
}

// Asleep: a slow bob up to SleepPx and back.
inline int SleepY(double t) {
    return t >= 0.0 ? -Round(SleepPx * Bump(t, SleepMs)) : 0;
}
// Paralysed: still, then a TwitchMs burst at the end of every TwitchEveryMs.
inline int TwitchX(double t) {
    if (!(t >= 0.0))
        return 0;
    const double phase = std::fmod(t, TwitchEveryMs) - (TwitchEveryMs - TwitchMs);
    if (phase < 0.0)
        return 0;
    return static_cast<int>(phase / TwitchStepMs) % 2 == 0 ? TwitchPx : -TwitchPx;
}
// Red HP: a slight sway, +-WobbleDeg every WobbleMs.
inline int WobbleDegAt(double t) {
    return t >= 0.0 ? Round(WobbleDeg * std::sin(2.0 * Pi * std::fmod(t, WobbleMs) / WobbleMs)) : 0;
}

enum class Loop { None, Sleep, Paralysis, LowHp };

// The status loop a battler shows: sleep (2) > paralysis (1) > red HP (hp <= 1/5).
inline Loop LoopOf(int status, int hp, int hp_max) {
    if (hp <= 0)
        return Loop::None;
    if (status == 2)
        return Loop::Sleep;
    if (status == 1)
        return Loop::Paralysis;
    return hp * 5 <= hp_max ? Loop::LowHp : Loop::None;
}

inline Pose LoopPose(Loop loop, double t) {
    Pose p;
    switch (loop) {
    case Loop::Sleep:
        p.y = SleepY(t);
        break;
    case Loop::Paralysis:
        p.x = TwitchX(t);
        break;
    case Loop::LowHp:
        p.rot = WobbleDegAt(t);
        break;
    case Loop::None:
        break;
    }
    return p;
}

// One battle sprite: fed every sample with what the companion shows for that spot.
class Battler {
public:
    // ident: who stands there (pokeID, species); hp: the shown HP (the top screen's gauge).
    void Observe(double now, std::int64_t ident, bool valid, int hp, int hp_max, int status) {
        if (!valid) {
            *this = Battler{};
            return;
        }
        if (ident != ident_) {
            // a new Pokémon in the spot (or the first sample): no hit; already fainted = sunk at once
            ident_ = ident;
            hit_ = drop_ = Never;
            fainted_ = hp == 0;
            faint_ = fainted_ ? now - FaintMs : Never;
        } else {
            if (hp < hp_) {
                if (now - drop_ > HitQuietMs)
                    hit_ = now;
                drop_ = now;
            }
            if (hp == 0 && hp_ > 0) {
                fainted_ = true;
                faint_ = now;
            } else if (hp > 0) {
                fainted_ = false;
            }
        }
        hp_ = hp;
        const Loop loop = LoopOf(status, hp, hp_max);
        if (loop != loop_) {
            loop_ = loop;
            loop_start_ = now;
        }
    }

    // faint > hit > (still while the gauge drains) > status loop
    Pose At(double now, int faint_px) const {
        if (ident_ == NoIdent)
            return {};
        if (fainted_)
            return Faint(now - faint_, faint_px);
        if (now - hit_ < HitMs)
            return HitShake(now - hit_);
        if (now - drop_ < HitQuietMs)
            return {}; // still draining after the shake: no status motion until the gauge stops
        return LoopPose(loop_, now - loop_start_);
    }

private:
    static constexpr std::int64_t NoIdent = std::numeric_limits<std::int64_t>::min();
    std::int64_t ident_ = NoIdent;
    int hp_ = -1;
    double drop_ = Never, hit_ = Never, faint_ = Never, loop_start_ = 0;
    bool fainted_ = false;
    Loop loop_ = Loop::None;
};

// The focused sprite of a page (the Party card, the Pokédex entry): hops when the selection
// changes or is tapped again, breathes while the page shows it.
class Focus {
public:
    void Observe(double now, std::int64_t key, bool visible) {
        if (visible && !visible_)
            breath_ = now; // starts at rest
        if (key != key_) {
            if (key_ != NoKey && visible)
                Hop(now);
            key_ = key;
        }
        visible_ = visible;
    }
    void Hop(double now) {
        hop_ = now;
        breath_ = now + HopMs; // the pulse restarts from rest after the hop
    }
    Pose At(double now) const {
        Pose p;
        if (!visible_)
            return p;
        p.y = HopY(now - hop_);
        p.scale = Breath(now - breath_);
        return p;
    }

private:
    static constexpr std::int64_t NoKey = std::numeric_limits<std::int64_t>::min();
    std::int64_t key_ = NoKey;
    bool visible_ = false;
    double hop_ = Never, breath_ = Never;
};

} // namespace lp_anim
