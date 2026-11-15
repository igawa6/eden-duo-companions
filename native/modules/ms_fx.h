// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary particle effects (the Spectral familiars' blue fire, Specter, Frosty,
// IceBlob's snow, the map teleporter sparkle ...): the game draws them with Unity ParticleSystems.
// pictures.idx carries each system's own parameters (P records, build_picture_index.py) and this
// file simulates them the way Unity does for the modules those systems use: rate emission or a
// single burst, birth sub-emitters, Circle / Box shapes, gravity, force, velocity / size / colour
// over lifetime (AnimationCurve Hermite keys, Gradient blend / fixed), limit velocity, billboards
// on the game's pixel grid. Unity seeds its randoms per run; here every random comes from a hash
// of (set, system, emission index), so a moment of the effect is a pure function of time and the
// pictures repeat every FxPeriod seconds.
#pragma once

#include "ms_mapdraw.h" // Picture

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ms {

inline constexpr double FxPeriod = 10.0; ///< seconds after which an effect repeats
/// Pictures per second of an effect shown on its own (the map teleporter sparkle). A monster's
/// effect steps with its idle clip instead (one picture per clip frame): every new picture is a
/// new host image key, which the host shows only once it has landed, so a key must be held
/// longer than a load takes (the idle clips' ~70 ms is known to hold).
inline constexpr int FxSteps = 15;
/// Defensive bounds of one DrawFx call: sub-emitter levels drawn under a root, and rate
/// emissions per root (the shipped pictures.idx needs a few hundred at most).
inline constexpr int FxMaxDepth = 3;
inline constexpr long long FxMaxEmissions = 4096;

/// An AnimationCurve (Unity Hermite keys, slopes per unit time; an infinite slope = a step).
struct FxCurve {
    struct Key {
        float t{}, v{}, in{}, out{};
    };
    std::vector<Key> keys;
    float Eval(float t) const;
};

/// A MinMaxCurve: constant, curve, random between two curves or two constants.
struct FxValue {
    enum class Mode { Constant, Curve, RandomCurves, RandomConstants } mode{Mode::Constant};
    float a{}, b{};
    FxCurve lo, hi;
    float Eval(float t, float random) const;
};

struct FxGradient {
    bool fixed{};
    std::vector<std::pair<float, std::array<float, 3>>> colors;
    std::vector<std::pair<float, float>> alphas;
    std::array<float, 4> Eval(float t) const;
};

struct FxSystem {
    int id{}, parent{-1};
    std::string texture; ///< "fx/RoundyPixel" etc. ("-" = none)
    float x{}, y{};      ///< root emitter position relative to the sprite anchor (px, y down)
    std::map<int, std::pair<float, float>> offsets; ///< MonsterParticleOffset by frame number
    bool loop{};
    float duration{}, rate{};
    int burst_min{}, burst_max{};
    float life[2]{}, speed[2]{}, size[2]{}, gravity{};
    float color0[4]{1, 1, 1, 1}, color1[4]{1, 1, 1, 1};
    enum class Shape { None, Box, Circle } shape{Shape::None};
    float shape_a{}, shape_b{}; ///< box: size x, y; circle: radius, thickness
    std::optional<FxCurve> size_curve;
    std::optional<FxValue> vx, vy;
    struct Limit {
        bool separate{};
        float x{}, y{};
        FxValue magnitude;
        float dampen{};
    };
    std::optional<Limit> limit;
    std::optional<FxGradient> gradient;
    float force[2]{};
    bool rendered{true};
    std::vector<int> subs; ///< systems whose burst fires at each of this one's particle births
};

/// One monster set's systems ("spectralwolf_light"), roots first in list order.
struct FxSet {
    std::vector<FxSystem> systems;
    /// Seconds a root emission stays visible (its own life plus its sub-emitters').
    double Reach(const FxSystem& root, int depth = 0) const;
};

/// Parses "P <set> <id> <parent> <texture> key=value ..." fields (f[0] == "P"); false on bad data.
bool ParseFxRecord(const std::vector<std::string_view>& f, std::map<std::string, FxSet, std::less<>>& sets);

/// Draws `set` at `seconds` (taken modulo FxPeriod) into `out`: the sprite anchor sits at
/// (anchor_x, anchor_y) picture px, one game pixel is `scale` picture px, `frame` is the number
/// after "/" in the shown frame's sprite name (MonsterParticleOffset). Alpha blended, clipped.
/// `texture` returns a texture by name (unscaled, top-left origin) or null.
void DrawFx(const FxSet& set, std::string_view set_name, double seconds, int frame, Picture& out,
            double anchor_x, double anchor_y, int scale,
            const std::function<const Picture*(std::string_view)>& texture);

/// The number after the last "/" of a sprite name ("SpectralWolf/7" -> 7), -1 when none.
int FrameNumber(std::string_view sprite);

} // namespace ms
