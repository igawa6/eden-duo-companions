// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Raw LZMA (LZMA1, no .lzma/.xz header) decoder for the Crash Team Racing Nitro-Fueled module.
// The game's igArchive blocks are "u32 packed size, 5-byte LZMA properties, raw stream"; each
// block decodes to a known size, so no end marker is needed. Written from the public LZMA
// specification; no dependency.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace CtrLzma {

/// Decodes `in` (the raw stream after the 5 property bytes) into exactly `out_size` bytes.
/// `props` is the 5-byte property block (lc/lp/pb byte + little-endian dictionary size).
/// Returns false on a malformed stream or when fewer than `out_size` bytes could be produced.
bool Decode(const std::uint8_t* props, const std::uint8_t* in, std::size_t in_size,
            std::size_t out_size, std::vector<std::uint8_t>& out);

} // namespace CtrLzma
