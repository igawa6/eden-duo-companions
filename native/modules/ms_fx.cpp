// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ms_fx.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ms {
namespace {

constexpr double Dt = 1.0 / 60.0;   // the game's frame: Unity steps its particles per frame
constexpr double Gravity = 9.81;    // Physics.gravity (0, -9.81); 1 world unit = 1 px here

bool Float(std::string_view s, float& out) {
    // "-0" and exponents come from Python's %g
    std::string tmp{s};
    char* end{};
    out = std::strtof(tmp.c_str(), &end);
    return end && *end == '\0' && !tmp.empty() && std::isfinite(out);
}

bool Int(std::string_view s, int& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (true) {
        const auto end = s.find(sep, pos);
        out.push_back(s.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        if (end == std::string_view::npos)
            return out;
        pos = end + 1;
    }
}

bool Floats(std::string_view s, float* out, std::size_t n) {
    const auto parts = Split(s, ',');
    if (parts.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
        if (!Float(parts[i], out[i]))
            return false;
    return true;
}

bool ParseCurve(std::string_view s, FxCurve& c) {
    for (const auto key : Split(s, ';')) {
        float v[4];
        if (!Floats(key, v, 4))
            return false;
        c.keys.push_back({v[0], v[1], v[2], v[3]});
    }
    return !c.keys.empty();
}

bool ParseValue(std::string_view s, FxValue& v) {
    if (s.starts_with("c:")) {
        v.mode = FxValue::Mode::Constant;
        return Float(s.substr(2), v.a);
    }
    if (s.starts_with("r:")) {
        float ab[2];
        v.mode = FxValue::Mode::RandomConstants;
        if (!Floats(s.substr(2), ab, 2))
            return false;
        v.a = ab[0];
        v.b = ab[1];
        return true;
    }
    if (s.starts_with("k:")) {
        v.mode = FxValue::Mode::Curve;
        return ParseCurve(s.substr(2), v.hi);
    }
    if (s.starts_with("rk:")) {
        v.mode = FxValue::Mode::RandomCurves;
        const auto bar = s.find('|');
        return bar != std::string_view::npos && ParseCurve(s.substr(3, bar - 3), v.lo) &&
               ParseCurve(s.substr(bar + 1), v.hi);
    }
    return false;
}

bool ParseGradient(std::string_view s, FxGradient& g) {
    const auto parts = Split(s, '/');
    int mode{};
    if (parts.size() != 3 || !Int(parts[0], mode))
        return false;
    g.fixed = mode == 1;
    for (const auto key : Split(parts[1], ';')) {
        float v[4];
        if (!Floats(key, v, 4))
            return false;
        g.colors.push_back({v[0], {v[1], v[2], v[3]}});
    }
    for (const auto key : Split(parts[2], ';')) {
        float v[2];
        if (!Floats(key, v, 2))
            return false;
        g.alphas.push_back({v[0], v[1]});
    }
    return !g.colors.empty() && !g.alphas.empty();
}

// splitmix64: every random of an effect is a hash of what it belongs to
std::uint64_t Mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct Rng {
    std::uint64_t state;
    float Next() {
        state = Mix(state);
        return static_cast<float>((state >> 40) * (1.0 / 16777216.0));
    }
};

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

struct Particle {
    double px{}, py{};  // position, y up, emitter-local px
    float size{};
    float rgba[4]{};
};

// Unity's start values for one particle, its birth place relative to the emitter, then its
// position `age` seconds later (y up).
struct Born {
    double x{}, y{}, vx{}, vy{};
    float life{}, size{}, color[4]{}, rvx{}, rvy{}, rlim{};
};

Born Birth(const FxSystem& s, Rng& rng, double at_x, double at_y) {
    Born b;
    b.life = Lerp(s.life[0], s.life[1], rng.Next());
    const float speed = Lerp(s.speed[0], s.speed[1], rng.Next());
    b.size = Lerp(s.size[0], s.size[1], rng.Next());
    const float c = rng.Next();
    for (int k = 0; k < 4; ++k)
        b.color[k] = Lerp(s.color0[k], s.color1[k], c);
    double ox = 0, oy = 0, dx = 0, dy = 0;
    if (s.shape == FxSystem::Shape::Box) {
        // the volume of a size-x by size-y box, emitting along +z (no visible start velocity)
        ox = (rng.Next() - 0.5) * s.shape_a;
        oy = (rng.Next() - 0.5) * s.shape_b;
    } else if (s.shape == FxSystem::Shape::Circle) {
        // the circle in the emitter's xy plane: thickness 1 = its whole area, 0 = its edge;
        // particles fly out from the centre
        const double a = rng.Next() * 2.0 * 3.14159265358979;
        const double inner = 1.0 - std::clamp<double>(s.shape_b, 0.0, 1.0);
        const double r = s.shape_a * std::sqrt(inner * inner + (1.0 - inner * inner) * rng.Next());
        dx = std::cos(a);
        dy = std::sin(a);
        ox = dx * r;
        oy = dy * r;
    }
    b.x = at_x + ox;
    b.y = at_y + oy;
    b.vx = dx * speed;
    b.vy = dy * speed;
    b.rvx = rng.Next();
    b.rvy = rng.Next();
    b.rlim = rng.Next();
    return b;
}

void Advance(const FxSystem& s, const Born& b, double age, double& x, double& y) {
    x = b.x;
    y = b.y;
    double vx = b.vx, vy = b.vy;
    double done = 0;
    while (done < age) {
        const double dt = std::min(Dt, age - done);
        const float t = static_cast<float>((done + dt * 0.5) / std::max(1e-6f, b.life));
        vy += -Gravity * s.gravity * dt + s.force[1] * dt;
        vx += s.force[0] * dt;
        if (s.limit) {
            const auto& l = *s.limit;
            const double damp = std::clamp<double>(l.dampen, 0.0, 1.0);
            if (l.separate) {
                const auto clampAxis = [damp](double v, double lim) {
                    if (std::abs(v) <= lim)
                        return v;
                    return v + ((v > 0 ? lim : -lim) - v) * damp;
                };
                vx = clampAxis(vx, l.x);
                vy = clampAxis(vy, l.y);
            } else {
                const double lim = l.magnitude.Eval(t, b.rlim);
                const double sp = std::hypot(vx, vy);
                if (sp > lim && sp > 0) {
                    const double k = 1.0 + (lim / sp - 1.0) * damp;
                    vx *= k;
                    vy *= k;
                }
            }
        }
        const double ax = s.vx ? s.vx->Eval(t, b.rvx) : 0.0;
        const double ay = s.vy ? s.vy->Eval(t, b.rvy) : 0.0;
        x += (vx + ax) * dt;
        y += (vy + ay) * dt;
        done += dt;
    }
}

Particle Look(const FxSystem& s, const Born& b, double age, double x, double y) {
    Particle p;
    p.px = x;
    p.py = y;
    const float t = static_cast<float>(age / std::max(1e-6f, b.life));
    p.size = b.size * (s.size_curve ? std::max(0.0f, s.size_curve->Eval(t)) : 1.0f);
    const auto g = s.gradient ? s.gradient->Eval(t) : std::array<float, 4>{1, 1, 1, 1};
    for (int k = 0; k < 4; ++k)
        p.rgba[k] = std::clamp(b.color[k] * g[k], 0.0f, 1.0f);
    return p;
}

std::uint64_t NameHash(std::string_view s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char ch : s)
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    return h;
}

} // namespace

float FxCurve::Eval(float t) const {
    if (keys.empty())
        return 0;
    if (t <= keys.front().t)
        return keys.front().v;
    if (t >= keys.back().t)
        return keys.back().v;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& k0 = keys[i];
        const auto& k1 = keys[i + 1];
        if (t > k1.t)
            continue;
        const float dt = k1.t - k0.t;
        if (dt <= 0 || !std::isfinite(k0.out) || !std::isfinite(k1.in))
            return k0.v; // a step key
        const float u = (t - k0.t) / dt, u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * k0.v + (u3 - 2 * u2 + u) * k0.out * dt + (-2 * u3 + 3 * u2) * k1.v +
               (u3 - u2) * k1.in * dt;
    }
    return keys.back().v;
}

float FxValue::Eval(float t, float random) const {
    switch (mode) {
    case Mode::Constant: return a;
    case Mode::RandomConstants: return Lerp(a, b, random);
    case Mode::Curve: return hi.Eval(t);
    case Mode::RandomCurves: return Lerp(lo.Eval(t), hi.Eval(t), random);
    }
    return 0;
}

std::array<float, 4> FxGradient::Eval(float t) const {
    std::array<float, 4> out{1, 1, 1, 1};
    const auto pick = [&](const auto& keys, auto&& lerp_value, auto&& assign) {
        if (keys.empty())
            return;
        if (fixed) {
            // Fixed: the colour of the first key at or after t
            for (const auto& k : keys)
                if (t <= k.first) {
                    assign(k.second);
                    return;
                }
            assign(keys.back().second);
            return;
        }
        if (t <= keys.front().first) {
            assign(keys.front().second);
            return;
        }
        for (std::size_t i = 0; i + 1 < keys.size(); ++i)
            if (t <= keys[i + 1].first) {
                const float span = keys[i + 1].first - keys[i].first;
                assign(lerp_value(keys[i].second, keys[i + 1].second, span > 0 ? (t - keys[i].first) / span : 0.0f));
                return;
            }
        assign(keys.back().second);
    };
    pick(colors,
         [](const std::array<float, 3>& a, const std::array<float, 3>& b, float u) {
             return std::array<float, 3>{Lerp(a[0], b[0], u), Lerp(a[1], b[1], u), Lerp(a[2], b[2], u)};
         },
         [&](const std::array<float, 3>& c) { out[0] = c[0], out[1] = c[1], out[2] = c[2]; });
    pick(alphas, [](float a, float b, float u) { return Lerp(a, b, u); }, [&](float a) { out[3] = a; });
    return out;
}

double FxSet::Reach(const FxSystem& root, int depth) const {
    double sub = 0;
    if (depth < FxMaxDepth) // DrawFx draws sub-emitters this deep, no deeper
        for (const int id : root.subs)
            if (id >= 0 && static_cast<std::size_t>(id) < systems.size())
                sub = std::max(sub, Reach(systems[static_cast<std::size_t>(id)], depth + 1));
    return root.life[1] + sub;
}

int FrameNumber(std::string_view sprite) {
    const auto slash = sprite.rfind('/');
    if (slash == std::string_view::npos)
        return -1;
    int n{};
    return Int(sprite.substr(slash + 1), n) ? n : -1;
}

bool ParseFxRecord(const std::vector<std::string_view>& f, std::map<std::string, FxSet, std::less<>>& sets) {
    if (f.size() < 5)
        return false;
    FxSystem s;
    if (!Int(f[2], s.id) || !Int(f[3], s.parent))
        return false;
    s.texture = std::string{f[4]};
    bool have_life = false;
    for (std::size_t i = 5; i < f.size(); ++i) {
        const auto eq = f[i].find('=');
        if (eq == std::string_view::npos)
            return false;
        const auto key = f[i].substr(0, eq), val = f[i].substr(eq + 1);
        int flag{};
        bool ok = true;
        if (key == "x")
            ok = Float(val, s.x);
        else if (key == "y")
            ok = Float(val, s.y);
        else if (key == "off") {
            for (const auto e : Split(val, ';')) {
                const auto colon = e.find(':');
                int n{};
                float xy[2];
                ok = ok && colon != std::string_view::npos && Int(e.substr(0, colon), n) && Floats(e.substr(colon + 1), xy, 2);
                if (ok)
                    s.offsets[n] = {xy[0], xy[1]};
            }
        } else if (key == "loop")
            ok = Int(val, flag), s.loop = flag != 0;
        else if (key == "dur")
            ok = Float(val, s.duration);
        else if (key == "rate")
            ok = Float(val, s.rate) && s.rate >= 0 && s.rate <= 1000;
        else if (key == "burst") {
            float mm[2];
            ok = Floats(val, mm, 2) && mm[0] >= 0 && mm[1] >= mm[0] && mm[1] <= 1000;
            s.burst_min = static_cast<int>(mm[0]);
            s.burst_max = static_cast<int>(mm[1]);
        } else if (key == "life")
            ok = Floats(val, s.life, 2) && s.life[0] > 0 && s.life[1] >= s.life[0] && s.life[1] <= 60, have_life = ok;
        else if (key == "speed")
            ok = Floats(val, s.speed, 2);
        else if (key == "size")
            ok = Floats(val, s.size, 2);
        else if (key == "grav")
            ok = Float(val, s.gravity);
        else if (key == "c0")
            ok = Floats(val, s.color0, 4);
        else if (key == "c1")
            ok = Floats(val, s.color1, 4);
        else if (key == "shape") {
            const auto p = Split(val, ',');
            ok = p.size() == 3 && Float(p[1], s.shape_a) && Float(p[2], s.shape_b);
            s.shape = p[0] == "box" ? FxSystem::Shape::Box : p[0] == "circle" ? FxSystem::Shape::Circle : FxSystem::Shape::None;
            ok = ok && s.shape != FxSystem::Shape::None;
        } else if (key == "sz") {
            FxCurve c;
            ok = ParseCurve(val, c);
            s.size_curve = std::move(c);
        } else if (key == "vx" || key == "vy") {
            FxValue v;
            ok = ParseValue(val, v);
            (key == "vx" ? s.vx : s.vy) = std::move(v);
        } else if (key == "lim") {
            // separate,lx,ly,<mmc with commas>,dampen: the value sits between the 3rd and last comma
            const auto c1 = val.find(','), c2 = val.find(',', c1 + 1), c3 = val.find(',', c2 + 1);
            const auto last = val.rfind(',');
            FxSystem::Limit l;
            ok = c3 != std::string_view::npos && last > c3 && Int(val.substr(0, c1), flag) &&
                 Float(val.substr(c1 + 1, c2 - c1 - 1), l.x) && Float(val.substr(c2 + 1, c3 - c2 - 1), l.y) &&
                 ParseValue(val.substr(c3 + 1, last - c3 - 1), l.magnitude) && Float(val.substr(last + 1), l.dampen);
            l.separate = flag != 0;
            s.limit = std::move(l);
        } else if (key == "grad") {
            FxGradient g;
            ok = ParseGradient(val, g);
            s.gradient = std::move(g);
        } else if (key == "force")
            ok = Floats(val, s.force, 2);
        else if (key == "ren")
            ok = Int(val, flag), s.rendered = flag != 0;
        // unknown keys: a newer index; ignored
        if (!ok)
            return false;
    }
    if (!have_life)
        return false;
    auto& set = sets[std::string{f[1]}];
    if (s.id != static_cast<int>(set.systems.size()) || s.parent >= s.id)
        return false;
    if (s.parent >= 0)
        set.systems[static_cast<std::size_t>(s.parent)].subs.push_back(s.id);
    set.systems.push_back(std::move(s));
    return true;
}

void DrawFx(const FxSet& set, std::string_view set_name, double seconds, int frame, Picture& out,
            double anchor_x, double anchor_y, int scale,
            const std::function<const Picture*(std::string_view)>& texture) {
    if (set.systems.empty() || out.rgba.empty() || scale < 1)
        return;
    const double t = std::fmod(std::fmod(seconds, FxPeriod) + FxPeriod, FxPeriod);
    const std::uint64_t seed = NameHash(set_name);
    std::vector<std::vector<Particle>> drawn(set.systems.size());

    // fires `s`'s burst at (x, y) (y up) at time `born`, recursing into its own birth sub-emitters
    const auto burst = [&](auto&& self, const FxSystem& s, std::uint64_t key, double born, double x, double y,
                           int depth) -> void {
        if (depth > FxMaxDepth)
            return;
        Rng rng{Mix(key ^ (static_cast<std::uint64_t>(s.id) << 48))};
        const int count = s.burst_min + static_cast<int>(rng.Next() * (s.burst_max - s.burst_min + 1));
        for (int j = 0; j < std::min(count, s.burst_max); ++j) {
            Rng prng{Mix(key + 0x1000u * static_cast<std::uint64_t>(j + 1) + static_cast<std::uint64_t>(s.id))};
            const Born b = Birth(s, prng, x, y);
            for (const int sub : s.subs)
                self(self, set.systems[static_cast<std::size_t>(sub)], Mix(key + 7919u * (j + 1)), born, b.x, b.y, depth + 1);
            const double age = t - born;
            if (age < 0 || age >= b.life || !s.rendered)
                continue;
            double px{}, py{};
            Advance(s, b, age, px, py);
            drawn[static_cast<std::size_t>(s.id)].push_back(Look(s, b, age, px, py));
        }
    };

    for (const auto& root : set.systems) {
        if (root.parent >= 0 || root.rate <= 0)
            continue;
        // rate emission: the k-th particle at k / rate on an endless timeline; its randoms repeat
        // every FxPeriod (round(rate * period) emissions)
        const long long period_count = std::max<long long>(1, std::llround(root.rate * FxPeriod));
        const double reach = set.Reach(root);
        const long long k1 = static_cast<long long>(std::floor(t * root.rate));
        // the shipped index needs a few hundred at most; a bad regen must not stall the worker
        const long long k0 = std::max(static_cast<long long>(std::floor((t - reach) * root.rate)), k1 - FxMaxEmissions);
        for (long long k = k0; k <= k1; ++k) {
            const double born = static_cast<double>(k) / root.rate;
            const long long slot = ((k % period_count) + period_count) % period_count;
            const std::uint64_t key = Mix(seed ^ (static_cast<std::uint64_t>(root.id) << 40) ^ static_cast<std::uint64_t>(slot));
            Rng rng{key};
            const Born b = Birth(root, rng, 0, 0);
            for (const int sub : root.subs)
                burst(burst, set.systems[static_cast<std::size_t>(sub)], key, born, b.x, b.y, 1);
            const double age = t - born;
            if (age < 0 || age >= b.life || !root.rendered)
                continue;
            double px{}, py{};
            Advance(root, b, age, px, py);
            drawn[static_cast<std::size_t>(root.id)].push_back(Look(root, b, age, px, py));
        }
    }

    // the emitter of each system: its root's position plus the offset of the frame shown
    std::vector<std::pair<double, double>> origin(set.systems.size());
    for (const auto& s : set.systems) {
        if (s.parent >= 0) {
            origin[static_cast<std::size_t>(s.id)] = origin[static_cast<std::size_t>(s.parent)];
            continue;
        }
        double ox = s.x, oy = s.y;
        if (const auto it = s.offsets.find(frame); it != s.offsets.end())
            ox += it->second.first, oy += it->second.second;
        origin[static_cast<std::size_t>(s.id)] = {ox, oy};
    }

    // billboards on the game's pixel grid (a pixel is covered when its centre is inside the quad),
    // textured, tinted, alpha blended; systems in index order, particles oldest first
    for (const auto& s : set.systems) {
        const Picture* tex = s.texture == "-" ? nullptr : texture(s.texture);
        for (const auto& p : drawn[static_cast<std::size_t>(s.id)]) {
            if (p.size <= 0 || p.rgba[3] <= 0)
                continue;
            const double cx = origin[static_cast<std::size_t>(s.id)].first + p.px;
            const double cy = origin[static_cast<std::size_t>(s.id)].second - p.py; // y down
            const double half = p.size * 0.5;
            const int gx0 = static_cast<int>(std::ceil(cx - half - 0.5)), gx1 = static_cast<int>(std::ceil(cx + half - 0.5));
            const int gy0 = static_cast<int>(std::ceil(cy - half - 0.5)), gy1 = static_cast<int>(std::ceil(cy + half - 0.5));
            for (int gy = gy0; gy < gy1; ++gy)
                for (int gx = gx0; gx < gx1; ++gx) {
                    float src[4] = {p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]};
                    if (tex && tex->width && tex->height) {
                        const double u = (gx + 0.5 - (cx - half)) / p.size, v = (gy + 0.5 - (cy - half)) / p.size;
                        const auto tx = std::clamp<int>(static_cast<int>(u * tex->width), 0, static_cast<int>(tex->width) - 1);
                        const auto ty = std::clamp<int>(static_cast<int>(v * tex->height), 0, static_cast<int>(tex->height) - 1);
                        const auto* texel = &tex->rgba[(static_cast<std::size_t>(ty) * tex->width + tx) * 4];
                        for (int k = 0; k < 4; ++k)
                            src[k] *= texel[k] / 255.0f;
                    }
                    if (src[3] <= 0)
                        continue;
                    const int px0 = static_cast<int>(std::lround(anchor_x)) + gx * scale;
                    const int py0 = static_cast<int>(std::lround(anchor_y)) + gy * scale;
                    for (int yy = std::max(py0, 0); yy < std::min(py0 + scale, static_cast<int>(out.height)); ++yy)
                        for (int xx = std::max(px0, 0); xx < std::min(px0 + scale, static_cast<int>(out.width)); ++xx) {
                            auto* d = &out.rgba[(static_cast<std::size_t>(yy) * out.width + xx) * 4];
                            const float a = src[3], da = d[3] / 255.0f, oa = a + da * (1 - a);
                            for (int k = 0; k < 3; ++k) {
                                const float c = (src[k] * a + (d[k] / 255.0f) * da * (1 - a)) / std::max(oa, 1e-6f);
                                d[k] = static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255));
                            }
                            d[3] = static_cast<std::uint8_t>(std::lround(std::clamp(oa, 0.0f, 1.0f) * 255));
                        }
                }
        }
    }
}

} // namespace ms
