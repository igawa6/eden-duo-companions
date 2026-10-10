// Portable scalar stand-ins for the SSE2 intrinsics used by ooz (kraken.cpp, lzna.cpp).
//
// Local modification for the Eden Duo companions (not part of upstream powzix/ooz): upstream
// includes <intrin.h> / <x86intrin.h>, which does not exist on Android arm64. Every intrinsic
// ooz uses is reimplemented here on plain bytes with the exact SSE2 lane semantics, and the same
// code is compiled on every platform, so the Linux tests exercise what Android runs. All loads
// and stores go through memcpy (no alignment or aliasing assumptions).
//
// The names are macros onto ooz_-prefixed definitions so nothing clashes with a compiler's own
// vector types if a system header happens to declare them.
#pragma once

#include <stdint.h>
#include <string.h>

struct ooz_m128i {
    uint8_t b[16];
};
struct ooz_m128 {
    uint8_t b[16];
};
struct ooz_m64 {
    uint8_t b[8];
};

#define __m128i ooz_m128i
#define __m128 ooz_m128
#define __m64 ooz_m64

static inline int16_t ooz_w(const ooz_m128i& v, int i) {
    int16_t r;
    memcpy(&r, v.b + 2 * i, 2);
    return r;
}
static inline void ooz_setw(ooz_m128i& v, int i, int16_t x) {
    memcpy(v.b + 2 * i, &x, 2);
}

static inline ooz_m128i ooz_mm_loadu_si128(const ooz_m128i* p) {
    ooz_m128i r;
    memcpy(r.b, p, 16);
    return r;
}
static inline void ooz_mm_storeu_si128(ooz_m128i* p, ooz_m128i v) {
    memcpy(p, v.b, 16);
}
static inline ooz_m128i ooz_mm_loadl_epi64(const ooz_m128i* p) {
    ooz_m128i r{};
    memcpy(r.b, p, 8);
    return r;
}
static inline void ooz_mm_storel_epi64(ooz_m128i* p, ooz_m128i v) {
    memcpy(p, v.b, 8);
}
static inline ooz_m128 ooz_mm_castsi128_ps(ooz_m128i v) {
    ooz_m128 r;
    memcpy(r.b, v.b, 16);
    return r;
}
// Stores the upper 64 bits.
static inline void ooz_mm_storeh_pi(ooz_m64* p, ooz_m128 v) {
    memcpy(p, v.b + 8, 8);
}
static inline ooz_m128i ooz_mm_set1_epi8(int8_t x) {
    ooz_m128i r;
    memset(r.b, static_cast<uint8_t>(x), 16);
    return r;
}
static inline ooz_m128i ooz_mm_set1_epi16(int16_t x) {
    ooz_m128i r;
    for (int i = 0; i < 8; ++i)
        ooz_setw(r, i, x);
    return r;
}
static inline ooz_m128i ooz_mm_set1_epi32(int32_t x) {
    ooz_m128i r;
    for (int i = 0; i < 4; ++i)
        memcpy(r.b + 4 * i, &x, 4);
    return r;
}
// _mm_set_epi16(e7, ..., e0): e0 is the lowest lane.
static inline ooz_m128i ooz_mm_set_epi16(int16_t e7, int16_t e6, int16_t e5, int16_t e4,
                                         int16_t e3, int16_t e2, int16_t e1, int16_t e0) {
    ooz_m128i r;
    const int16_t e[8] = {e0, e1, e2, e3, e4, e5, e6, e7};
    for (int i = 0; i < 8; ++i)
        ooz_setw(r, i, e[i]);
    return r;
}
static inline ooz_m128i ooz_mm_cvtsi32_si128(int32_t x) {
    ooz_m128i r{};
    memcpy(r.b, &x, 4);
    return r;
}
static inline ooz_m128i ooz_mm_add_epi8(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] = static_cast<uint8_t>(a.b[i] + b.b[i]);
    return a;
}
static inline ooz_m128i ooz_mm_sub_epi8(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] = static_cast<uint8_t>(a.b[i] - b.b[i]);
    return a;
}
static inline ooz_m128i ooz_mm_add_epi16(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 8; ++i)
        ooz_setw(a, i, static_cast<int16_t>(static_cast<uint16_t>(ooz_w(a, i)) +
                                             static_cast<uint16_t>(ooz_w(b, i))));
    return a;
}
static inline ooz_m128i ooz_mm_sub_epi16(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 8; ++i)
        ooz_setw(a, i, static_cast<int16_t>(static_cast<uint16_t>(ooz_w(a, i)) -
                                             static_cast<uint16_t>(ooz_w(b, i))));
    return a;
}
static inline ooz_m128i ooz_mm_and_si128(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] &= b.b[i];
    return a;
}
static inline ooz_m128i ooz_mm_xor_si128(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] ^= b.b[i];
    return a;
}
static inline ooz_m128i ooz_mm_cmpeq_epi8(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] = a.b[i] == b.b[i] ? 0xFF : 0x00;
    return a;
}
// Signed 16-bit compare.
static inline ooz_m128i ooz_mm_cmpgt_epi16(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 8; ++i)
        ooz_setw(a, i, ooz_w(a, i) > ooz_w(b, i) ? static_cast<int16_t>(-1) : 0);
    return a;
}
static inline ooz_m128i ooz_mm_max_epu8(ooz_m128i a, ooz_m128i b) {
    for (int i = 0; i < 16; ++i)
        a.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i];
    return a;
}
static inline int ooz_mm_movemask_epi8(ooz_m128i a) {
    int m = 0;
    for (int i = 0; i < 16; ++i)
        m |= (a.b[i] >> 7) << i;
    return m;
}
// Signed saturation of a's then b's eight words into sixteen bytes.
static inline ooz_m128i ooz_mm_packs_epi16(ooz_m128i a, ooz_m128i b) {
    ooz_m128i r;
    for (int i = 0; i < 16; ++i) {
        const int v = i < 8 ? ooz_w(a, i) : ooz_w(b, i - 8);
        r.b[i] = static_cast<uint8_t>(static_cast<int8_t>(v < -128 ? -128 : v > 127 ? 127 : v));
    }
    return r;
}
// Byte shift left by n (towards higher lanes), zero fill.
static inline ooz_m128i ooz_mm_slli_si128_impl(ooz_m128i a, int n) {
    ooz_m128i r{};
    if (n < 16)
        memcpy(r.b + n, a.b, 16 - n);
    return r;
}
#define _mm_slli_si128(a, n) ooz_mm_slli_si128_impl((a), (n))
// Logical right shift of each 16-bit lane.
static inline ooz_m128i ooz_mm_srli_epi16(ooz_m128i a, int n) {
    for (int i = 0; i < 8; ++i)
        ooz_setw(a, i, static_cast<int16_t>(n > 15 ? 0 : static_cast<uint16_t>(ooz_w(a, i)) >> n));
    return a;
}
// Arithmetic right shift of each 16-bit lane.
static inline ooz_m128i ooz_mm_srai_epi16(ooz_m128i a, int n) {
    if (n > 15)
        n = 15;
    for (int i = 0; i < 8; ++i) {
        const int v = ooz_w(a, i);
        ooz_setw(a, i, static_cast<int16_t>(v < 0 ? ~(~v >> n) : v >> n));
    }
    return a;
}
static inline ooz_m128i ooz_mm_unpacklo_epi8(ooz_m128i a, ooz_m128i b) {
    ooz_m128i r;
    for (int i = 0; i < 8; ++i) {
        r.b[2 * i] = a.b[i];
        r.b[2 * i + 1] = b.b[i];
    }
    return r;
}
static inline ooz_m128i ooz_mm_unpackhi_epi8(ooz_m128i a, ooz_m128i b) {
    ooz_m128i r;
    for (int i = 0; i < 8; ++i) {
        r.b[2 * i] = a.b[8 + i];
        r.b[2 * i + 1] = b.b[8 + i];
    }
    return r;
}
static inline ooz_m128i ooz_mm_unpacklo_epi16(ooz_m128i a, ooz_m128i b) {
    ooz_m128i r;
    for (int i = 0; i < 4; ++i) {
        ooz_setw(r, 2 * i, ooz_w(a, i));
        ooz_setw(r, 2 * i + 1, ooz_w(b, i));
    }
    return r;
}
// Dword shuffle: result lane i = a lane ((imm >> 2i) & 3).
static inline ooz_m128i ooz_mm_shuffle_epi32_impl(ooz_m128i a, int imm) {
    ooz_m128i r;
    for (int i = 0; i < 4; ++i)
        memcpy(r.b + 4 * i, a.b + 4 * ((imm >> (2 * i)) & 3), 4);
    return r;
}
#define _mm_shuffle_epi32(a, imm) ooz_mm_shuffle_epi32_impl((a), (imm))

#define _MM_HINT_T0 3
#if defined(__GNUC__) || defined(__clang__)
#define _mm_prefetch(p, hint) __builtin_prefetch((p))
#else
#define _mm_prefetch(p, hint) ((void)(p))
#endif

#define _mm_loadu_si128 ooz_mm_loadu_si128
#define _mm_storeu_si128 ooz_mm_storeu_si128
#define _mm_loadl_epi64 ooz_mm_loadl_epi64
#define _mm_storel_epi64 ooz_mm_storel_epi64
#define _mm_castsi128_ps ooz_mm_castsi128_ps
#define _mm_storeh_pi ooz_mm_storeh_pi
#define _mm_set1_epi8 ooz_mm_set1_epi8
#define _mm_set1_epi16 ooz_mm_set1_epi16
#define _mm_set1_epi32 ooz_mm_set1_epi32
#define _mm_set_epi16 ooz_mm_set_epi16
#define _mm_cvtsi32_si128 ooz_mm_cvtsi32_si128
#define _mm_add_epi8 ooz_mm_add_epi8
#define _mm_sub_epi8 ooz_mm_sub_epi8
#define _mm_add_epi16 ooz_mm_add_epi16
#define _mm_sub_epi16 ooz_mm_sub_epi16
#define _mm_and_si128 ooz_mm_and_si128
#define _mm_xor_si128 ooz_mm_xor_si128
#define _mm_cmpeq_epi8 ooz_mm_cmpeq_epi8
#define _mm_cmpgt_epi16 ooz_mm_cmpgt_epi16
#define _mm_max_epu8 ooz_mm_max_epu8
#define _mm_movemask_epi8 ooz_mm_movemask_epi8
#define _mm_packs_epi16 ooz_mm_packs_epi16
#define _mm_srli_epi16 ooz_mm_srli_epi16
#define _mm_srai_epi16 ooz_mm_srai_epi16
#define _mm_unpacklo_epi8 ooz_mm_unpacklo_epi8
#define _mm_unpackhi_epi8 ooz_mm_unpackhi_epi8
#define _mm_unpacklo_epi16 ooz_mm_unpacklo_epi16
