// C entry point over ooz's Kraken_Decompress (local addition for the Eden Duo companions; see
// README.md). Returns the number of bytes written (== dst_len on success) or -1.
//
// ooz is NOT fuzz safe and may write up to 64 bytes past dst + dst_len. Callers must hash-check
// the compressed input against a trusted value first and pass a destination buffer with at least
// OOZ_SAFE_SPACE bytes of slack (the companions' wrapper, dq3_ooz.cpp, does both).
#include "stdafx.h"

int Kraken_Decompress(const byte* src, size_t src_len, byte* dst, size_t dst_len);

extern "C" int ooz_decompress(const unsigned char* src, size_t src_len, unsigned char* dst,
                              size_t dst_len) {
    return Kraken_Decompress(src, src_len, dst, dst_len);
}
