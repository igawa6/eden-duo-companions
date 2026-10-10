// ooz C entry point (see oozlib.cpp and README.md).
#pragma once

#include <stddef.h>

/// ooz may write this many bytes past the end of the destination buffer.
#define OOZ_SAFE_SPACE 64

extern "C" int ooz_decompress(const unsigned char* src, size_t src_len, unsigned char* dst,
                              size_t dst_len);
