// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dread_mapgen_util.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>

namespace dread_mapgen {
namespace {

// ---- inflate (RFC 1951) ------------------------------------------------------------------------
// Structure after Mark Adler's puff.c (zlib licence, see LICENSES/Zlib.txt), altered: C++ types,
// span input and overrun flags.

struct BitReader {
    std::span<const std::uint8_t> in;
    std::size_t pos = 0;
    std::uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool overrun = false;
    std::size_t max_out = std::numeric_limits<std::size_t>::max(); // output cap (Inflate max_out)

    int Bits(int need) {
        std::uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= in.size()) {
                overrun = true;
                return 0;
            }
            val |= std::uint32_t(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return int(val & ((1u << need) - 1u));
    }
};

constexpr int MaxBits = 15;

struct Huffman {
    std::array<std::uint16_t, MaxBits + 1> count{};
    std::array<std::uint16_t, 320> symbol{};
};

// Canonical code from code lengths. Returns false for an over-subscribed set; an incomplete set
// is accepted (only a single-code distance tree is legitimately incomplete in practice).
bool Build(Huffman& h, const std::uint16_t* length, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s)
        ++h.count[length[s]];
    if (h.count[0] == n)
        return true;
    int left = 1;
    for (int len = 1; len <= MaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return false;
    }
    std::array<std::uint16_t, MaxBits + 1> offs{};
    for (int len = 1; len < MaxBits; ++len)
        offs[len + 1] = std::uint16_t(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0)
            h.symbol[offs[length[s]]++] = std::uint16_t(s);
    return true;
}

int Decode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MaxBits; ++len) {
        code |= br.Bits(1);
        if (br.overrun)
            return -1;
        const int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr std::array<std::uint16_t, 29> LenBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                                15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                                67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> LenExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> DistBase{
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> DistExtra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(BitReader& br, std::vector<std::uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = Decode(br, lit);
        if (sym < 0)
            return false;
        if (sym < 256) {
            if (out.size() >= br.max_out)
                return false;
            out.push_back(std::uint8_t(sym));
            continue;
        }
        if (sym == 256)
            return true;
        sym -= 257;
        if (sym >= 29)
            return false;
        const std::size_t len = LenBase[sym] + std::size_t(br.Bits(LenExtra[sym]));
        const int dsym = Decode(br, dist);
        if (dsym < 0 || dsym >= 30 || br.overrun)
            return false;
        const std::size_t d = DistBase[dsym] + std::size_t(br.Bits(DistExtra[dsym]));
        if (br.overrun || d > out.size() || len > br.max_out - out.size())
            return false;
        const std::size_t from = out.size() - d;
        for (std::size_t i = 0; i < len; ++i)
            out.push_back(out[from + i]);
    }
}

bool Fixed(BitReader& br, std::vector<std::uint8_t>& out) {
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        std::array<std::uint16_t, 288> l{};
        int s = 0;
        for (; s < 144; ++s)
            l[s] = 8;
        for (; s < 256; ++s)
            l[s] = 9;
        for (; s < 280; ++s)
            l[s] = 7;
        for (; s < 288; ++s)
            l[s] = 8;
        Build(t.first, l.data(), 288);
        std::array<std::uint16_t, 30> d{};
        d.fill(5);
        Build(t.second, d.data(), 30);
        return t;
    }();
    return Codes(br, out, tables.first, tables.second);
}

bool Dynamic(BitReader& br, std::vector<std::uint8_t>& out) {
    const int nlen = br.Bits(5) + 257;
    const int ndist = br.Bits(5) + 1;
    const int ncode = br.Bits(4) + 4;
    if (br.overrun || nlen > 286 || ndist > 30)
        return false;
    static constexpr std::array<std::uint8_t, 19> Order{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                         11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::array<std::uint16_t, 320> lengths{};
    for (int i = 0; i < ncode; ++i)
        lengths[Order[i]] = std::uint16_t(br.Bits(3));
    Huffman lencode;
    if (br.overrun || !Build(lencode, lengths.data(), 19))
        return false;
    lengths.fill(0);
    for (int index = 0; index < nlen + ndist;) {
        int sym = Decode(br, lencode);
        if (sym < 0)
            return false;
        if (sym < 16) {
            lengths[index++] = std::uint16_t(sym);
            continue;
        }
        std::uint16_t len = 0;
        int repeat;
        if (sym == 16) {
            if (index == 0)
                return false;
            len = lengths[index - 1];
            repeat = 3 + br.Bits(2);
        } else if (sym == 17) {
            repeat = 3 + br.Bits(3);
        } else {
            repeat = 11 + br.Bits(7);
        }
        if (br.overrun || index + repeat > nlen + ndist)
            return false;
        while (repeat--)
            lengths[index++] = len;
    }
    if (lengths[256] == 0)
        return false;
    Huffman lit, dist;
    if (!Build(lit, lengths.data(), nlen) || !Build(dist, lengths.data() + nlen, ndist))
        return false;
    return Codes(br, out, lit, dist);
}

bool Stored(BitReader& br, std::vector<std::uint8_t>& out) {
    br.bitbuf = 0;
    br.bitcnt = 0;
    if (br.pos + 4 > br.in.size())
        return false;
    const unsigned len = br.in[br.pos] | br.in[br.pos + 1] << 8;
    const unsigned nlen = br.in[br.pos + 2] | br.in[br.pos + 3] << 8;
    br.pos += 4;
    if (len != (~nlen & 0xFFFFu) || br.pos + len > br.in.size() || len > br.max_out - out.size())
        return false;
    out.insert(out.end(), br.in.begin() + std::ptrdiff_t(br.pos),
               br.in.begin() + std::ptrdiff_t(br.pos + len));
    br.pos += len;
    return true;
}

} // namespace

bool Inflate(std::span<const std::uint8_t> deflate, std::vector<std::uint8_t>& out,
             std::size_t max_out) {
    if (out.size() > max_out)
        return false;
    BitReader br{deflate};
    br.max_out = max_out;
    for (;;) {
        const int last = br.Bits(1);
        const int type = br.Bits(2);
        if (br.overrun)
            return false;
        bool ok = false;
        if (type == 0)
            ok = Stored(br, out);
        else if (type == 1)
            ok = Fixed(br, out);
        else if (type == 2)
            ok = Dynamic(br, out);
        if (!ok)
            return false;
        if (last)
            return true;
    }
}

bool Gunzip(std::span<const std::uint8_t> gz, std::vector<std::uint8_t>& out) {
    if (gz.size() < 18 || gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 8)
        return false;
    const std::uint8_t flags = gz[3];
    std::size_t at = 10;
    if (flags & 4) { // FEXTRA
        if (at + 2 > gz.size())
            return false;
        at += 2 + std::size_t(gz[at] | gz[at + 1] << 8);
    }
    for (const std::uint8_t bit : {std::uint8_t(8), std::uint8_t(16)}) { // FNAME, FCOMMENT
        if (flags & bit) {
            while (at < gz.size() && gz[at] != 0)
                ++at;
            ++at;
        }
    }
    if (flags & 2) // FHCRC
        at += 2;
    if (at >= gz.size())
        return false;
    out.clear();
    return Inflate(gz.subspan(at), out);
}

double PyRound1(double x) {
    if (!std::isfinite(x) || std::fabs(x) >= 0x1p49)
        return x; // no double this large has a fractional digit to round (and none occur here)
    // CPython: _Py_dg_dtoa(x, mode 3, ndigits 1) -> the correctly rounded decimal string (ties to
    // even on the exact binary value), then strtod. std::to_chars with a precision rounds exactly
    // like printf("%.1f") in the "C" locale. Reading the string back: its digits without the point
    // are an integer N with |N| < 2^53, exact as a double, and the IEEE quotient N / 10.0 is the
    // double nearest to N/10 -- what strtod returns -- without strtod's locale dependence (and
    // without from_chars(double), which the NDK's libc++ lacks).
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::fixed, 1);
    if (res.ec != std::errc{})
        return x;
    long long n = 0;
    bool negative = false;
    for (const char* p = buf; p != res.ptr; ++p) {
        if (*p == '-')
            negative = true;
        else if (*p >= '0' && *p <= '9')
            n = n * 10 + (*p - '0');
    }
    const double q = double(n) / 10.0;
    return negative ? -q : q; // "-0.0" stays -0.0, as Python's round(-0.04, 1)
}

namespace {
struct DoubleLength {
    double hi, lo;
};
// Error-free product and fast two-sum, as CPython's mathmodule.c (dl_mul with fma, dl_fast_sum).
DoubleLength DlMul(double x, double y) {
    const double hi = x * y;
    return {hi, std::fma(x, y, -hi)};
}
DoubleLength DlFastSum(double a, double b) {
    const double x = a + b;
    const double z = x - a;
    return {x, b - z};
}
} // namespace

double PyHypot(double x, double y) {
    // CPython 3.12+ math.hypot -> vector_norm(), transcribed for n == 2 (finite inputs).
    const double ax = std::fabs(x), ay = std::fabs(y);
    if (std::isinf(ax) || std::isinf(ay))
        return std::numeric_limits<double>::infinity();
    if (std::isnan(ax) || std::isnan(ay))
        return std::numeric_limits<double>::quiet_NaN();
    const double max = ax > ay ? ax : ay;
    if (max == 0.0)
        return max;
    int max_e = 0;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        constexpr double DblMin = std::numeric_limits<double>::min();
        return DblMin * PyHypot(ax / DblMin, ay / DblMin);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (const double v : {ax, ay}) {
        const double s = v * scale;
        const DoubleLength pr = DlMul(s, s);
        const DoubleLength sm = DlFastSum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    const DoubleLength pr = DlMul(-h, h);
    const DoubleLength sm = DlFastSum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double r = csum - 1.0 + (frac1 + frac2);
    h += r / (2.0 * h);
    return h / scale;
}

} // namespace dread_mapgen
