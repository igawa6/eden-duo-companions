// Portable replacement for upstream ooz's Windows-only stdafx.h (local modification for the Eden
// Duo companions; see README.md). Upstream includes <Windows.h>, <tchar.h> and <intrin.h>; this
// shim provides the same typedefs and the handful of MSVC intrinsics ooz calls, with compiler
// builtins only, so the sources build unchanged on Linux x86-64 and Android arm64. The SSE2
// intrinsics come from ooz_simd.h (scalar, identical on every platform).
#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ooz_simd.h"

typedef unsigned char byte;
typedef unsigned char uint8;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef signed long long int64;
typedef signed int int32;
typedef unsigned short uint16;
typedef signed short int16;
typedef unsigned int uint;

// MSVC _BitScanReverse/_BitScanForward: index of the highest/lowest set bit; 0 when m == 0
// (the index is then left untouched, as on MSVC).
static inline unsigned char _BitScanReverse(unsigned long* i, uint32 m) {
    if (!m)
        return 0;
    *i = 31 - __builtin_clz(m);
    return 1;
}
static inline unsigned char _BitScanForward(unsigned long* i, uint32 m) {
    if (!m)
        return 0;
    *i = __builtin_ctz(m);
    return 1;
}
static inline unsigned char _BitScanReverse(uint32* i, uint32 m) {
    if (!m)
        return 0;
    *i = 31 - __builtin_clz(m);
    return 1;
}
static inline unsigned char _BitScanForward(uint32* i, uint32 m) {
    if (!m)
        return 0;
    *i = __builtin_ctz(m);
    return 1;
}
static inline unsigned char _BitScanReverse(int* i, uint32 m) {
    if (!m)
        return 0;
    *i = 31 - __builtin_clz(m);
    return 1;
}
static inline unsigned char _BitScanForward(int* i, uint32 m) {
    if (!m)
        return 0;
    *i = __builtin_ctz(m);
    return 1;
}

static inline uint16 _byteswap_ushort(uint16 v) {
    return __builtin_bswap16(v);
}
static inline uint32 _byteswap_ulong(uint32 v) {
    return __builtin_bswap32(v);
}
static inline uint64 _byteswap_uint64(uint64 v) {
    return __builtin_bswap64(v);
}
static inline uint32 _rotl(uint32 v, int n) {
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}
static inline uint64 _rotl64(uint64 v, int n) {
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}

#define __forceinline inline __attribute__((always_inline))
#define _countof(a) (sizeof(a) / sizeof(*(a)))
