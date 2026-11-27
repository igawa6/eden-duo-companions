// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_maptiles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace xcx::tiles {
namespace {
constexpr double Pi = 3.14159265358979323846;
Box FromTexels(double u0, double v0, double u1, double v1) {
    const double x0 = u0 / map::WorldScale - map::WorldHalfX, x1 = u1 / map::WorldScale - map::WorldHalfX;
    const double z0 = v0 / map::WorldScale - map::WorldHalfZ, z1 = v1 / map::WorldScale - map::WorldHalfZ;
    return {x0, -z1, x1, -z0};
}
} // namespace
bool Intersects(const Box &b, const ViewRect &v, double margin) {
    const double hw = v.w * (0.5 + margin), hh = v.h * (0.5 + margin);
    return b.x1 > v.cx - hw && b.x0 < v.cx + hw && b.y1 > v.cy - hh && b.y0 < v.cy + hh;
}
std::optional<Point> BarracksWidgetPoint(double x, double z) {
    if (!std::isfinite(x) || !std::isfinite(z))
        return std::nullopt;
    const auto t = map::BarracksUv(x, z);
    const Point p{t.u - BarracksU0, double(BarracksV0 + BarracksH) - t.v};
    if (p.x < 0 || p.y < 0 || p.x > BarracksW || p.y > BarracksH)
        return std::nullopt;
    return p;
}
Box HiBox(int tx, int ty) { return FromTexels(tx * HiW, ty * HiH, (tx + 1) * HiW, ty * HiH + HiRowHeight(ty)); }
Box LoBox(int tx, int ty) {
    return FromTexels(tx * 3072., ty * 2816., (tx + 1) * 3072., (ty + 1) * 2816.);
}
Box NlaBox(int tx, int ty) {
    const double x0 = NlaX0 + tx * NlaTileMetres, z0 = NlaZ0 + ty * NlaTileMetres;
    return {x0, -(z0 + NlaTileMetres), x0 + NlaTileMetres, -z0};
}
std::string KeyText(const Key &k) {
    static constexpr const char *names[] = {"h", "l", "n", "b", "blank", "arrow", "skell"};
    char text[96];
    std::snprintf(text, sizeof text, "module:xcx:map/%s/%d/%d/%s", names[int(k.set)], k.tx, k.ty,
                  k.gen.empty() ? "0" : k.gen.c_str());
    return text;
}
std::optional<Key> ParseKey(std::string_view s) {
    constexpr std::string_view prefix = "module:xcx:map/";
    if (!s.starts_with(prefix) || s.size() > 80)
        return std::nullopt;
    const std::string text(s.substr(prefix.size()));
    char name[16]{}, gen[48]{};
    int tx = 0, ty = 0;
    if (std::sscanf(text.c_str(), "%15[a-z]/%d/%d/%47s", name, &tx, &ty, gen) != 4)
        return std::nullopt;
    Key k;
    k.tx = tx;
    k.ty = ty;
    k.gen = gen;
    const std::string_view n = name;
    int cols = 1, rows = 1;
    if (n == "h") { k.set = Set::Hi; cols = HiCols; rows = HiRows; }
    else if (n == "l") { k.set = Set::Lo; cols = LoCols; rows = LoRows; }
    else if (n == "n") { k.set = Set::Nla; cols = NlaCols; rows = NlaRows; }
    else if (n == "b") k.set = Set::Barracks;
    else if (n == "blank") k.set = Set::Blank;
    else if (n == "arrow") k.set = Set::Arrow;
    else if (n == "skell") k.set = Set::Skell;
    else return std::nullopt;
    if (tx < 0 || ty < 0 || tx >= cols || ty >= rows)
        return std::nullopt;
    for (char c : k.gen)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-'))
            return std::nullopt;
    return k;
}
std::vector<int> Needed(Set set, const ViewRect &view, double margin) {
    std::vector<int> out;
    int cols = 0, rows = 0;
    Box (*box)(int, int) = nullptr;
    switch (set) {
    case Set::Hi: cols = HiCols; rows = HiRows; box = HiBox; break;
    case Set::Lo: cols = LoCols; rows = LoRows; box = LoBox; break;
    case Set::Nla: cols = NlaCols; rows = NlaRows; box = NlaBox; break;
    default: return out;
    }
    for (int ty = 0; ty < rows; ++ty)
        for (int tx = 0; tx < cols; ++tx)
            if (Intersects(box(tx, ty), view, margin))
                out.push_back(ty * cols + tx);
    return out;
}
std::vector<int> Nearest(Set set, const ViewRect &view, double margin, std::size_t limit) {
    auto list = Needed(set, view, margin);
    const int cols = set == Set::Hi ? HiCols : set == Set::Lo ? LoCols : NlaCols;
    const auto centre = [&](int n) {
        const Box b = set == Set::Hi ? HiBox(n % cols, n / cols) : set == Set::Lo ? LoBox(n % cols, n / cols) : NlaBox(n % cols, n / cols);
        return std::hypot((b.x0 + b.x1) / 2 - view.cx, (b.y0 + b.y1) / 2 - view.cy);
    };
    std::sort(list.begin(), list.end(), [&](int a, int b) { return centre(a) < centre(b); });
    if (list.size() > limit)
        list.resize(limit);
    return list;
}
std::vector<int> NlaNeed(const ViewRect &view, double player_x, double player_y) {
    auto need = Nearest(Set::Nla, view, .25, MaxVisible);
    if (std::isfinite(player_x) && std::isfinite(player_y))
        for (const int n : Nearest(Set::Nla, {player_x, player_y, 40., 40.}, 0., 4))
            if (need.size() < MaxVisible && std::find(need.begin(), need.end(), n) == need.end())
                need.push_back(n);
    return need;
}
Widget WidgetFor(int mode, map::Kind kind) {
    if (mode == 0) // Overworld: Mira around the player, or the whole of it where there is no position
        return kind == map::Kind::World || kind == map::Kind::Nla ? Widget::Overworld : Widget::OverworldHome;
    switch (kind) {
    case map::Kind::World: return Widget::AreaWorld;
    case map::Kind::Nla: return Widget::AreaNla;
    case map::Kind::Barracks: return Widget::AreaBarracks;
    default: return Widget::AreaNone;
    }
}
int HeadingDegrees(double yaw) {
    if (!std::isfinite(yaw))
        return 0;
    double d = std::fmod(yaw * 180. / Pi, 360.);
    if (d < 0)
        d += 360.;
    const int whole = int(std::lround(d));
    return whole % 360;
}
Pose ChoosePose(const ActorPose &pc, const ActorPose &skell, const Pose &last) {
    const ActorPose *a = skell.present && skell.driven ? &skell : pc.driven ? &pc : nullptr;
    if (!a)
        return last; // transition: keep what was last known
    return {a->yaw_valid && std::isfinite(a->yaw), a->x, a->z, a->yaw};
}
} // namespace xcx::tiles
