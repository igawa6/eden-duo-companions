// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_icon_assets.h"
#include "acnh_tex.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
namespace echoes {
namespace {
constexpr std::string_view Archive = "region_common/ui/GameMain.arc#";
constexpr size_t MaxMetadata = 256u << 10, MaxTexture = 32u << 20;
struct B {
  std::span<const uint8_t> b;
  void Fit(size_t o, size_t n) const {
    if (o > b.size() || n > b.size() - o)
      throw std::runtime_error("icon range");
  }
  uint8_t U8(size_t o) const {
    Fit(o, 1);
    return b[o];
  }
  uint16_t U16(size_t o) const {
    Fit(o, 2);
    return uint16_t(b[o] | uint16_t(b[o + 1]) << 8);
  }
  uint32_t U32(size_t o) const {
    Fit(o, 4);
    return b[o] | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 |
           uint32_t(b[o + 3]) << 24;
  }
  double F(size_t o) const {
    auto u = U32(o);
    float f;
    std::memcpy(&f, &u, 4);
    if (!std::isfinite(f) || std::abs(f) > 1e7)
      throw std::runtime_error("icon float");
    return f;
  }
  B Sub(size_t o, size_t n) const {
    Fit(o, n);
    return {b.subspan(o, n)};
  }
  bool Tag(std::string_view s) const {
    return b.size() >= 4 && !std::memcmp(b.data(), s.data(), 4);
  }
  std::string Str(size_t o, size_t n) const {
    Fit(o, n);
    size_t z = 0;
    while (z < n && b[o + z])
      ++z;
    if (z == n)
      throw std::runtime_error("icon string");
    return std::string(reinterpret_cast<const char *>(b.data() + o), z);
  }
};
struct Srt {
  double x{}, y{}, r{}, sx{1}, sy{1};
};
struct Material {
  std::string name;
  std::array<uint8_t, 4> black{}, white{};
  std::vector<std::string> textures;
  std::vector<Srt> srt;
};
struct Pane {
  std::string name;
  int parent{-1}, material{-1};
  double x{}, y{}, r{}, sx{1}, sy{1}, w{}, h{};
  uint8_t alpha{255}, flags{1};
  std::array<std::array<uint8_t, 4>, 4> vertex{};
  std::vector<std::array<double, 8>> uv;
};
struct Layout {
  std::vector<std::string> textures;
  std::vector<Material> materials;
  std::vector<Pane> panes;
};
void Header(B b, std::string_view magic) {
  if (!b.Tag(magic) || b.U16(4) != 0xfeff || b.U16(6) < 20 ||
      b.U32(12) != b.b.size() || b.b.size() > MaxMetadata)
    throw std::runtime_error("icon header");
}
Layout ParseLayout(const std::vector<uint8_t> &data) {
  B b{data};
  Header(b, "FLYT");
  Layout out;
  std::vector<int> stack{-1};
  int last = -1;
  size_t count = 0;
  for (size_t o = b.U16(6); o < b.b.size();) {
    auto n = b.U32(o + 4);
    if (n < 8)
      throw std::runtime_error("icon section");
    auto s = b.Sub(o, n);
    ++count;
    if (s.Tag("txl1")) {
      auto num = s.U16(8);
      if (num > 64 || !out.textures.empty())
        throw std::runtime_error("icon textures");
      s.Fit(12, num * 4);
      for (size_t i = 0; i < num; ++i) {
        size_t a = 12 + uint64_t(s.U32(12 + i * 4));
        out.textures.push_back(s.Str(a, std::min(size_t(128), n - a)));
      }
    } else if (s.Tag("mat1")) {
      auto num = s.U16(8);
      if (num > 64 || !out.materials.empty())
        throw std::runtime_error("icon materials");
      s.Fit(12, num * 4);
      for (size_t i = 0; i < num; ++i) {
        size_t a = s.U32(12 + i * 4), end = i + 1 < num ? s.U32(16 + i * 4) : n;
        if (a < 12 + size_t(num) * 4 || end < a)
          throw std::runtime_error("icon material offsets");
        auto m = s.Sub(a, end - a);
        Material v;
        v.name = m.Str(0, 28);
        auto flags = m.U32(28);
        for (size_t c = 0; c < 4; ++c) {
          v.black[c] = m.U8(36 + c);
          v.white[c] = m.U8(40 + c);
        }
        size_t nt = flags & 3, ns = (flags >> 2) & 3;
        if (nt == 0 || ns != nt)
          throw std::runtime_error("icon material srt");
        for (size_t t = 0; t < nt; ++t) {
          auto ti = m.U16(44 + t * 4);
          if (ti >= out.textures.size())
            throw std::runtime_error("icon texture index");
          v.textures.push_back(out.textures[ti]);
        }
        for (size_t t = 0; t < ns; ++t) {
          size_t q = 44 + nt * 4 + t * 20;
          v.srt.push_back(
              {m.F(q), m.F(q + 4), m.F(q + 8), m.F(q + 12), m.F(q + 16)});
        }
        out.materials.push_back(std::move(v));
      }
    } else if (s.Tag("pas1")) {
      if (last < 0 || stack.size() >= 32)
        throw std::runtime_error("icon hierarchy");
      stack.push_back(last);
    } else if (s.Tag("pae1")) {
      if (stack.size() < 2)
        throw std::runtime_error("icon hierarchy end");
      stack.pop_back();
    } else if (s.Tag("pan1") || s.Tag("pic1") || s.Tag("bnd1")) {
      if (out.panes.size() >= 64)
        throw std::runtime_error("icon pane count");
      Pane p;
      p.name = s.Str(12, 24);
      p.parent = stack.back();
      p.flags = s.U8(8);
      if (s.U8(9) != 0)
        throw std::runtime_error("unsupported icon origin");
      p.alpha = s.U8(10);
      p.x = s.F(44);
      p.y = s.F(48);
      p.r = s.F(64);
      p.sx = s.F(68);
      p.sy = s.F(72);
      p.w = s.F(76);
      p.h = s.F(80);
      if (p.w < 0 || p.h < 0)
        throw std::runtime_error("icon size");
      if (s.Tag("pic1")) {
        p.material = s.U16(100);
        if (p.material >= int(out.materials.size()))
          throw std::runtime_error("icon material reference");
        for (size_t v = 0; v < 4; ++v)
          for (size_t c = 0; c < 4; ++c)
            p.vertex[v][c] = s.U8(84 + v * 4 + c);
        auto num = s.U8(102);
        if (num == 0 || num > 3)
          throw std::runtime_error("icon UV count");
        for (size_t t = 0; t < num; ++t) {
          std::array<double, 8> uv{};
          for (size_t q = 0; q < 8; ++q)
            uv[q] = s.F(104 + t * 32 + q * 4);
          p.uv.push_back(uv);
        }
      }
      out.panes.push_back(std::move(p));
      last = int(out.panes.size() - 1);
    }
    o += n;
  }
  if (count != b.U16(16) || stack.size() != 1 || out.panes.empty())
    throw std::runtime_error("icon structure");
  return out;
}
struct Track {
  std::string name, tag;
  uint8_t kind{}, index{}, target{};
  std::vector<MapIconCurveKey> keys;
};
struct Loop {
  uint16_t frames{};
  std::vector<Track> tracks;
};
Loop ParseLoop(const std::vector<uint8_t> &data) {
  B b{data};
  Header(b, "FLAN");
  Loop out;
  bool found = false;
  size_t count = 0;
  for (size_t o = b.U16(6); o < b.b.size();) {
    auto n = b.U32(o + 4);
    if (n < 8)
      throw std::runtime_error("icon animation section");
    auto s = b.Sub(o, n);
    ++count;
    if (s.Tag("pai1")) {
      if (found)
        throw std::runtime_error("duplicate icon loop");
      found = true;
      out.frames = s.U16(8);
      if (out.frames == 0 || out.frames > 10000 || s.U8(10) != 1)
        throw std::runtime_error("icon duration");
      auto num = s.U16(14);
      auto offsets = s.U32(16);
      if (num > 64)
        throw std::runtime_error("icon animation count");
      s.Fit(offsets, num * 4);
      for (size_t e = 0; e < num; ++e) {
        size_t a = s.U32(offsets + 4 * e);
        s.Fit(a, 32);
        auto name = s.Str(a, 28);
        auto nt = s.U8(a + 28), kind = s.U8(a + 29);
        if (nt > 16 || kind > 1)
          throw std::runtime_error("icon animation entry");
        s.Fit(a + 32, nt * 4);
        for (size_t i = 0; i < nt; ++i) {
          size_t t = a + uint64_t(s.U32(a + 32 + 4 * i));
          s.Fit(t, 8);
          std::string tag(reinterpret_cast<const char *>(s.b.data() + t), 4);
          auto nn = s.U8(t + 4);
          if (nn > 32)
            throw std::runtime_error("icon track count");
          s.Fit(t + 8, nn * 4);
          for (size_t k = 0; k < nn; ++k) {
            size_t q = t + uint64_t(s.U32(t + 8 + 4 * k));
            s.Fit(q, 12);
            Track v;
            v.name = name;
            v.tag = tag;
            v.kind = kind;
            v.index = s.U8(q);
            v.target = s.U8(q + 1);
            auto curve = s.U8(q + 2);
            auto nk = s.U16(q + 4);
            if (curve != 2 || nk == 0 || nk > 128)
              throw std::runtime_error("icon curve format");
            size_t keys = q + uint64_t(s.U32(q + 8));
            s.Fit(keys, nk * 12);
            for (size_t x = 0; x < nk; ++x) {
              MapIconCurveKey key{s.F(keys + x * 12), s.F(keys + x * 12 + 4),
                                  s.F(keys + x * 12 + 8)};
              if (!v.keys.empty() && key.frame < v.keys.back().frame)
                throw std::runtime_error("icon key order");
              v.keys.push_back(key);
            }
            if (out.tracks.size() >= 128)
              throw std::runtime_error("icon total tracks");
            out.tracks.push_back(std::move(v));
          }
        }
      }
    }
    o += n;
  }
  if (!found || count != b.U16(16) || out.tracks.empty())
    throw std::runtime_error("icon animation structure");
  return out;
}
std::array<double, 4> Sample(const Image &im, double u, double v, bool repeat) {
  if (!std::isfinite(u) || !std::isfinite(v))
    throw std::runtime_error("icon sampling coordinate");
  if (repeat) {
    u -= std::floor(u);
    v -= std::floor(v);
  } else {
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
  }
  double x = u * im.w - .5, y = v * im.h - .5;
  int ix = int(std::floor(x)), iy = int(std::floor(y));
  double fx = x - ix, fy = y - iy;
  std::array<double, 4> result{};
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      int a = std::clamp(ix + dx, 0, im.w - 1),
          b = std::clamp(iy + dy, 0, im.h - 1);
      auto p = im.At(a, b);
      double weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      for (size_t c = 0; c < 4; ++c)
        result[c] += p[c] * weight / 255.;
    }
  return result;
}
void Over(uint8_t *d, std::array<double, 4> s) {
  double sa = std::clamp(s[3], 0., 1.), da = d[3] / 255.,
         a = sa + da * (1 - sa);
  if (a <= 0)
    return;
  for (int c = 0; c < 3; ++c)
    d[c] = uint8_t(std::clamp(
        std::lround((s[c] * sa + d[c] / 255. * da * (1 - sa)) / a * 255), 0l,
        255l));
  d[3] = uint8_t(std::clamp(std::lround(a * 255), 0l, 255l));
}
} // namespace
bool EvaluateMapIconCurve(std::span<const MapIconCurveKey> keys, double f,
                          double &value) {
  value = 0;
  if (keys.empty() || keys.size() > 128 || !std::isfinite(f))
    return false;
  for (size_t i = 0; i < keys.size(); ++i)
    if (!std::isfinite(keys[i].frame) || !std::isfinite(keys[i].value) ||
        !std::isfinite(keys[i].slope) ||
        (i && keys[i].frame < keys[i - 1].frame))
      return false;
  auto it = std::upper_bound(
      keys.begin(), keys.end(), f,
      [](double t, const MapIconCurveKey &k) { return t < k.frame; });
  if (it == keys.begin()) {
    value = keys.front().value;
    return true;
  }
  auto a = *(it - 1);
  if (it == keys.end()) {
    value = a.value;
    return true;
  }
  auto b = *it;
  double dt = b.frame - a.frame;
  if (dt <= 0)
    return false;
  double t = (f - a.frame) / dt, t2 = t * t, t3 = t2 * t;
  value = (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * dt * a.slope +
          (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * dt * b.slope;
  return std::isfinite(value);
}
struct MapIconAssets::Impl {
  ReadAssetFn read;
  Layout checkpoint, base;
  Loop loop;
  std::map<std::string, std::vector<uint8_t>> sources;
  std::map<std::string, Image> textures;
  bool ready{};
  explicit Impl(ReadAssetFn fn) : read(std::move(fn)) {}
  const std::vector<uint8_t> &Source(const std::string &path, size_t bound) {
    auto it = sources.find(path);
    if (it != sources.end())
      return it->second;
    size_t n = read ? read(path, 0, nullptr, 0) : 0;
    if (!n || n > bound)
      throw std::runtime_error("icon missing source");
    std::vector<uint8_t> data(n);
    if (read(path, 0, data.data(), n) != n)
      throw std::runtime_error("icon short source");
    return sources.emplace(path, std::move(data)).first->second;
  }
  const Image &Texture(std::string name, bool iconmap = false) {
    std::string key = (iconmap ? "I:" : "G:") + name;
    auto it = textures.find(key);
    if (it != textures.end())
      return it->second;
    Image out;
    const auto path = std::string(
        iconmap ? "region_common/ui/IconMap.arc#timg/__Combined.bntx"
                : "region_common/ui/GameMain.arc#timg/__Combined.bntx");
    if (!acnh::BntxDecode(Source(path, MaxTexture), name, out) || out.Empty() ||
        out.w > 2048 || out.h > 2048)
      throw std::runtime_error("icon texture decode");
    return textures.emplace(key, std::move(out)).first->second;
  }
  void Prepare() {
    if (ready)
      return;
    checkpoint = ParseLayout(Source(
        std::string(Archive) + "blyt/MapIcon_CheckPoint.bflyt", MaxMetadata));
    loop = ParseLoop(
        Source(std::string(Archive) + "anim/MapIcon_CheckPoint_Loop.bflan",
               MaxMetadata));
    if (loop.frames != 700 || loop.tracks.size() != 53)
      throw std::runtime_error("unsupported checkpoint loop");
    base = ParseLayout(
        Source(std::string(Archive) + "blyt/MapIcon_Base.bflyt", MaxMetadata));
    ready = true;
  }
  void Render(Layout layout, double frame, uint32_t side, Image &out,
              std::string_view native_icon) {
    const bool static_icon = !native_icon.empty();
    if (!static_icon)
      for (const auto &t : loop.tracks) {
        double v;
        if (!EvaluateMapIconCurve(t.keys, frame, v))
          throw std::runtime_error("icon key evaluation");
        if (t.kind == 0) {
          auto p =
              std::find_if(layout.panes.begin(), layout.panes.end(),
                           [&](const auto &p) { return p.name == t.name; });
          if (p == layout.panes.end())
            throw std::runtime_error("icon animation pane");
          if (t.tag == "FLPA") {
            if (t.target == 6)
              p->sx = v;
            else if (t.target == 7)
              p->sy = v;
            else
              throw std::runtime_error("unsupported pane curve");
          } else if (t.tag == "FLVC" && t.target == 16)
            p->alpha = uint8_t(std::clamp(std::lround(v), 0l, 255l));
          else
            throw std::runtime_error("unsupported pane animation");
        } else if (t.tag == "FLTS") {
          auto m =
              std::find_if(layout.materials.begin(), layout.materials.end(),
                           [&](const auto &m) { return m.name == t.name; });
          if (m == layout.materials.end() || t.index >= m->srt.size())
            throw std::runtime_error("icon srt target");
          auto &s = m->srt[t.index];
          switch (t.target) {
          case 0:
            s.x = v;
            break;
          case 1:
            s.y = v;
            break;
          case 2:
            s.r = v;
            break;
          case 3:
            s.sx = v;
            break;
          case 4:
            s.sy = v;
            break;
          default:
            throw std::runtime_error("icon srt channel");
          }
        } else if (t.tag != "FLMC")
          throw std::runtime_error("unsupported material curve");
      }
    out = Image(int(side), int(side));
    const double scale =
        side / (static_icon ? 40. : 64.); // one uniform scale, native aspect
    for (const auto &pane : layout.panes) {
      if (pane.material < 0)
        continue;
      if (!static_icon && pane.name != "P_Icon_01" &&
          pane.name != "P_IconEff_00")
        continue;
      auto mat = layout.materials[size_t(pane.material)];
      if (static_icon)
        mat.textures[0] = std::string(native_icon);
      double alpha = 1, sx = scale, sy = scale;
      const Pane *p = &pane;
      double px = 0, py = 0;
      while (p) {
        if (!(p->flags & 1)) {
          alpha = 0;
          break;
        }
        alpha *= p->alpha / 255.;
        sx *= p->sx;
        sy *= p->sy;
        px += p->x;
        py += p->y;
        p = p->parent < 0 ? nullptr : &layout.panes[size_t(p->parent)];
      }
      if (alpha <= 0 || pane.w <= 0 || pane.h <= 0 || !std::isfinite(sx) ||
          !std::isfinite(sy) || std::abs(sx) < 1e-9 || std::abs(sy) < 1e-9)
        continue;
      std::vector<const Image *> tex;
      for (const auto &name : mat.textures)
        tex.push_back(&Texture(name, static_icon));
      for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x) {
          double u = ((x + .5 - side / 2.) - px * scale) / (pane.w * sx) + .5,
                 v = ((y + .5 - side / 2.) + py * scale) / (pane.h * sy) + .5;
          if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > 1 ||
              v < 0 || v > 1)
            continue;
          std::array<double, 4> color{};
          for (size_t ti = 0; ti < tex.size(); ++ti) {
            const auto s = mat.srt[ti];
            const auto &uv = pane.uv[std::min(ti, pane.uv.size() - 1)];
            const double native_u = uv[0] * (1 - u) * (1 - v) +
                                    uv[2] * u * (1 - v) + uv[4] * (1 - u) * v +
                                    uv[6] * u * v;
            const double native_v = uv[1] * (1 - u) * (1 - v) +
                                    uv[3] * u * (1 - v) + uv[5] * (1 - u) * v +
                                    uv[7] * u * v;
            double du = (native_u - .5) * s.sx, dv = (native_v - .5) * s.sy,
                   r = s.r * 3.14159265358979323846 / 180.;
            double tu = du * std::cos(r) - dv * std::sin(r) + .5 + s.x,
                   tv = du * std::sin(r) + dv * std::cos(r) + .5 + s.y;
            auto sample = Sample(*tex[ti], tu, tv, ti > 0);
            if (ti == 0)
              color = sample;
            else
              color[3] *= sample[0];
          }
          for (size_t c = 0; c < 4; ++c) {
            double vertex =
                (pane.vertex[0][c] * (1 - u) * (1 - v) +
                 pane.vertex[1][c] * u * (1 - v) +
                 pane.vertex[2][c] * (1 - u) * v + pane.vertex[3][c] * u * v) /
                255.;
            color[c] = (mat.black[c] / 255. +
                        (mat.white[c] - mat.black[c]) / 255. * color[c]) *
                       vertex;
          }
          color[3] *= alpha;
          Over(out.At(int(x), int(y)), color);
        }
    }
  }
};
MapIconAssets::MapIconAssets(ReadAssetFn read)
    : impl_(std::make_unique<Impl>(std::move(read))) {}
MapIconAssets::~MapIconAssets() = default;
bool MapIconAssets::Checkpoint(double frame, uint32_t side, Image &out,
                               MapIconRenderInfo *info) {
  out = {};
  if (info)
    *info = {};
  if (!std::isfinite(frame) || frame < 0 || side < 40 || side > 256)
    return false;
  try {
    impl_->Prepare();
    frame = std::fmod(frame, impl_->loop.frames);
    impl_->Render(impl_->checkpoint, frame, side, out, {});
    if (info)
      *info = {impl_->loop.frames, uint32_t(impl_->loop.tracks.size()), 4, true,
               false};
    return !out.Empty();
  } catch (...) {
    out = {};
    return false;
  }
}
bool MapIconAssets::Smoothie(uint32_t side, Image &out) {
  out = {};
  if (side < 40 || side > 256)
    return false;
  try {
    impl_->Prepare();
    impl_->Render(impl_->base, 0, side, out, "Smoothie");
    return !out.Empty();
  } catch (...) {
    out = {};
    return false;
  }
}
bool MapIconAssets::Landmark(std::string_view texture, uint32_t side,
                             Image &out) {
  out = {};
  if (side < 40 || side > 256 || texture.empty() || texture.size() > 64 ||
      texture.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") !=
          std::string_view::npos)
    return false;
  try {
    impl_->Prepare();
    impl_->Render(impl_->base, 0, side, out, texture);
    return !out.Empty();
  } catch (...) {
    out = {};
    return false;
  }
}
bool MapIconAssets::Chest(bool open, uint32_t side, Image &out) {
  out = {};
  if (side < 40 || side > 256)
    return false;
  try {
    impl_->Prepare();
    impl_->Render(impl_->base, 0, side, out,
                  open ? "TreasureBoxOpen" : "TreasureBox");
    return !out.Empty();
  } catch (...) {
    out = {};
    return false;
  }
}
} // namespace echoes
