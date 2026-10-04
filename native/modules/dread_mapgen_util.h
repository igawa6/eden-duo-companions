// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Small, dependency-free helpers of the Dread map generator (dread_mapgen.cpp):
//   - Inflate / Gunzip: RFC 1951 inflate and a gzip-member wrapper (the maproom models keep their
//     vertex/index buffers gzip-compressed).
//   - PyRound1 / PyHypot: Python's round(x, 1) and a correctly rounded hypot, so the generated
//     numbers are bit-identical to the Python bakers that produced the 1.0.0 package data.
// Pure functions, any thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dread_mapgen {

/// Raw deflate stream -> bytes. False on a malformed stream, or once `out` would grow past
/// max_out bytes (checked before each append, so a corrupt stream cannot allocate more).
bool Inflate(std::span<const std::uint8_t> deflate, std::vector<std::uint8_t>& out,
             std::size_t max_out = SIZE_MAX);
/// One gzip member (RFC 1952 header, deflate body). False on a malformed member.
bool Gunzip(std::span<const std::uint8_t> gz, std::vector<std::uint8_t>& out);

/// CPython's round(x, 1): the double nearest to x correctly rounded (half-even on the exact binary
/// value) to one decimal place.
double PyRound1(double x);

/// sqrt(x*x + y*y) correctly rounded (double-double sum, corrected square root). Agrees with
/// CPython's math.hypot, which is correctly rounded for two arguments.
double PyHypot(double x, double y);

} // namespace dread_mapgen
