# ooz (vendored)

Open-source Kraken / Mermaid / Selkie / Leviathan / LZNA / BitKnit decompressor by Powzix.

- Upstream: https://github.com/powzix/ooz, commit `05038060aa68f9187ae9923b2388ca8db40e58d1`
  (2019-02-13).
- License: GNU General Public License version 3 or (at your option) any later version, as stated
  in the upstream `kraken.cpp` header (kept unchanged at the top of `kraken.cpp`). The full license
  text is in [COPYING](COPYING). The companions repository itself is GPLv3.
- Used by: the Dragon Quest III HD-2D Remake module (`dsmod-dq3`) to decode Oodle-compressed
  members of the player's own `Nicola-Switch.pak`. Nothing decoded is shipped.

Upstream warns that ooz is **not fuzz safe**. The module therefore only feeds it pak members
whose stored compressed bytes match a build-pinned SHA-1 (and the archive's own entry hash), with
bounded, exact destination sizes plus 64 bytes of slack (`dq3_pak.cpp`).

## Local modifications

| File | Change |
|---|---|
| `kraken.cpp` | Upstream file truncated after `Kraken_Decompress`: the Windows command-line tool (`error`, `load_file`, option parsing, `Verify`, the `oo2core` DLL loader and `main`) is removed. The decoder code above it is byte-for-byte upstream. |
| `lzna.cpp`, `bitknit.cpp` | Unchanged upstream copies. |
| `stdafx.h` | New. Replaces upstream's Windows-only `stdafx.h` (`<Windows.h>`, `<tchar.h>`, `<intrin.h>`, `targetver.h`): the same integer typedefs, and `_BitScanReverse/Forward`, `_byteswap_*`, `_rotl/_rotl64`, `__forceinline`, `_countof` implemented with compiler builtins only. |
| `ooz_simd.h` | New. Scalar implementations of the SSE2 intrinsics ooz uses, with the exact SSE2 lane semantics, compiled on every platform (there is no `x86intrin.h` on Android arm64). `_mm_prefetch` maps to `__builtin_prefetch`. |
| `oozlib.cpp`, `ooz.h` | New. `extern "C" ooz_decompress(src, src_len, dst, dst_len)` over `Kraken_Decompress`. |

The sources are compiled with `-fno-strict-aliasing` (ooz type-puns through pointer casts) and
`-fno-tree-vectorize`: GCC 16's loop vectorizer at `-O3` miscompiles the overlapping 8-byte LZ
copies (wrong output for one block of a B8G8R8A8 sprite atlas, caught by the module's decoded-hash
check; Clang 22 and GCC `-O2` were correct).

## Verification

The portable build (this directory, scalar SIMD shim) was compared with the reference Oodle
library on the Switch 1.1.0.0 merged archive: byte-identical output on 280 extracted UI/font/map
members (666 blocks) and on a random archive-wide sample of 2000 members (2545 blocks, seed
20261005). The `dsmod-dq3` unit test also decodes canned Kraken blocks.
