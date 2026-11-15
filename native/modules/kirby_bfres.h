// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

// Shared pieces for drawing the game's BFRES v9 models in software: vectors, 3 x 4 matrices and a bounds-checked
// reader with the vertex attribute formats the models use.
namespace KirbyBfres {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct V3 {
    float x{}, y{}, z{};
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Unit(V3 a) {
    const float n = std::sqrt(Dot(a, a));
    return n > 0 ? a * (1 / n) : a;
}

// 3 x 4, row major
using M34 = std::array<float, 12>;
constexpr M34 Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

inline M34 Mul(const M34& a, const M34& b) {
    M34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        r[i * 4 + 3] += a[i * 4 + 3];
    }
    return r;
}

inline V3 Point(const M34& m, V3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

inline V3 Dir(const M34& m, V3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z, m[4] * p.x + m[5] * p.y + m[6] * p.z, m[8] * p.x + m[9] * p.y + m[10] * p.z};
}

inline M34 Inverse(const M34& m) {
    const auto a = [&](int i, int j) { return m[i * 4 + j]; };
    const float det = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) - a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
                      a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    if (det == 0)
        return Identity;
    M34 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 4 + j] = (a((j + 1) % 3, (i + 1) % 3) * a((j + 2) % 3, (i + 2) % 3) -
                            a((j + 1) % 3, (i + 2) % 3) * a((j + 2) % 3, (i + 1) % 3)) / det;
    for (int i = 0; i < 3; ++i)
        r[i * 4 + 3] = -(r[i * 4] * m[3] + r[i * 4 + 1] * m[7] + r[i * 4 + 2] * m[11]);
    return r;
}

inline float Half(u16 h) {
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v = exp == 0 ? std::ldexp(static_cast<float>(man), -24)
              : exp == 31 ? 65504.0f
                          : std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

class Reader {
public:
    explicit Reader(std::span<const u8> d) : d(d) {}
    bool ok = true;
    std::span<const u8> d;

    template <typename T>
    T At(u64 o) {
        T v{};
        if (o > d.size() || sizeof(T) > d.size() - o) {
            ok = false;
            return v;
        }
        std::memcpy(&v, d.data() + o, sizeof v);
        return v;
    }
    u64 Q(u64 o) { return At<u64>(o); }
    u32 W(u64 o) { return At<u32>(o); }
    u16 H(u64 o) { return At<u16>(o); }
    std::string Name(u64 o) {
        const u16 n = H(o);
        if (!ok || o + 2 + n > d.size())
            return ok = false, std::string{};
        return {reinterpret_cast<const char*>(d.data() + o + 2), n};
    }
    // a vertex attribute as up to four floats; false for a format the models do not use
    bool Attribute(u16 fmt, u64 o, std::array<float, 4>& v, int& n) {
        if (o + 16 > d.size())
            return false;
        const u8* p = d.data() + o;
        const auto s8 = [](u8 b) { return std::max(-1.0f, static_cast<float>(static_cast<std::int8_t>(b)) / 127); };
        switch (fmt) {
        case 0x1805:
        case 0x1705:
            n = fmt == 0x1805 ? 3 : 2;
            std::memcpy(v.data(), p, 4 * n);
            return true;
        case 0x1205:
        case 0x1505:
            n = fmt == 0x1205 ? 2 : 4;
            for (int i = 0; i < n; ++i) {
                u16 h;
                std::memcpy(&h, p + 2 * i, 2);
                v[i] = Half(h);
            }
            return true;
        case 0x1201:
        case 0x1202:
            n = 2;
            for (int i = 0; i < 2; ++i) {
                u16 h;
                std::memcpy(&h, p + 2 * i, 2);
                v[i] = fmt == 0x1201 ? static_cast<float>(h) / 65535 : std::max(-1.0f, static_cast<float>(static_cast<std::int16_t>(h)) / 32767);
            }
            return true;
        case 0x0E02: {
            u32 w;
            std::memcpy(&w, p, 4);
            const auto s10 = [](u32 x) { return static_cast<float>(x & 512 ? static_cast<int>(x) - 1024 : static_cast<int>(x)) / 511; };
            n = 3, v = {s10(w & 1023), s10(w >> 10 & 1023), s10(w >> 20 & 1023), 0};
            return true;
        }
        case 0x0B01:
        case 0x0901:
            n = fmt == 0x0B01 ? 4 : 2;
            for (int i = 0; i < n; ++i)
                v[i] = static_cast<float>(p[i]) / 255;
            return true;
        case 0x0201:
            n = 1, v[0] = static_cast<float>(p[0]) / 255;
            return true;
        case 0x0203:
        case 0x0903:
        case 0x0B03:
            // bone indices
            n = fmt == 0x0203 ? 1 : fmt == 0x0903 ? 2 : 4;
            for (int i = 0; i < n; ++i)
                v[i] = static_cast<float>(p[i]);
            return true;
        case 0x0B02:
        case 0x0902:
            n = fmt == 0x0B02 ? 4 : 2;
            for (int i = 0; i < n; ++i)
                v[i] = s8(p[i]);
            return true;
        default:
            return false;
        }
    }
};
} // namespace KirbyBfres
