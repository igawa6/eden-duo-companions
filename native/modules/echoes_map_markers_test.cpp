// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_markers.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool b, const char *s) {
  ++checks;
  if (!b) {
    std::cerr << "FAIL " << s << '\n';
    std::exit(1);
  }
}
uint32_t U32(const std::vector<uint8_t> &b, size_t p) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i)
    v |= uint32_t(b.at(p + i)) << (8 * i);
  return v;
}
void Put(std::vector<uint8_t> &b, size_t p, uint32_t v, size_t n = 4) {
  for (size_t i = 0; i < n; ++i)
    b.at(p + i) = uint8_t(v >> (8 * i));
}
} // namespace
int main() {
  std::vector<NativeCheckpointGroup> out(1);
  Check(!ParseNativeCheckpoints({}, "Field", out) && out.empty(),
        "empty input clears output");
  Check(!LoadNativeCheckpoints({}, "Field", out) && out.empty(),
        "missing reader clears output");
  const auto root = std::getenv("ECHOES_ROMFS");
  if (!root) {
    std::cout << checks << " unit checks passed; fixture missing\n";
    return 77;
  }
  std::ifstream f(std::filesystem::path(root) /
                      "region_common/level/Common.eclo",
                  std::ios::binary);
  const std::vector<uint8_t> original{std::istreambuf_iterator<char>(f), {}};
  Check(ParseNativeCheckpoints(original, "Field", out) && out.size() == 73,
        "original Field groups");
  const auto expected = out;
  size_t markers = 0, cat1 = 0, cat3 = 0;
  for (const auto &g : out) {
    for (const auto &m : g.markers) {
      ++markers;
      for (const auto &c : m.conditions) {
        cat1 += c.category == 1;
        cat3 += c.category == 3;
      }
    }
  }
  Check(markers == 153 && cat1 == 32 && cat3 == 149,
        "original marker and condition counts");
  const auto read = [&](std::string_view path, uint64_t off, void *p,
                        size_t n) -> size_t {
    if (path != "region_common/level/Common.eclo")
      return 0;
    if (!p && !n)
      return original.size();
    if (off > original.size() || n > original.size() - off)
      return 0;
    std::copy_n(original.data() + off, n, static_cast<uint8_t *>(p));
    return n;
  };
  Check(LoadNativeCheckpoints(read, "Field", out) && out == expected,
        "worker original catalog roundtrip");
  Check(!ParseNativeCheckpoints(original, "unknown", out) && out.empty(),
        "unknown level fails closed");
  for (size_t n : {size_t(0), size_t(4), size_t(0x8b), original.size() - 1}) {
    auto b = original;
    b.resize(n);
    out = expected;
    Check(!ParseNativeCheckpoints(b, "Field", out) && out.empty(),
          "truncation clears all metadata");
  }
  auto Reject = [&](size_t p, uint32_t v, size_t n = 4) {
    auto b = original;
    Put(b, p, v, n);
    out = expected;
    Check(!ParseNativeCheckpoints(b, "Field", out) && out.empty(),
          "corrupt source rejected");
  };
  Reject(4, 0);
  Reject(8, 1);
  Reject(0x84, 0);
  Reject(0x88, 0);
  Reject(0x74, 0xffffffff);
  Reject(0x7c, 0);
  Reject(0x10, 0xffffffff);
  Reject(0x18, 0xffffffff);
  Reject(0x44, 4097);
  Reject(0x48, 0xffffffff);
  Reject(0x4c, 16385);
  Reject(0x50, 0xffffffff);
  Reject(0x54, 65537);
  Reject(0x58, 0xffffffff);
  Reject(0x78, 0xffffffff);
  Reject(0x80, 0xffffffff);
  const auto group = U32(original, 0x48), marker = U32(original, 0x50),
             expr = U32(original, 0x58), dir = U32(original, 0x10);
  Reject(group, 0);
  Reject(group + 4, 0xffffffff);
  Reject(group + 8, 0xffffffff);
  Reject(marker, 0x7fc00000);
  Reject(marker + 12, 0x7f800000);
  Reject(marker + 27, 2, 1);
  Reject(marker + 22, 65535, 2);
  Reject(marker + 24, 65535, 2);
  Reject(expr + 4, 0xffffffff);
  Reject(dir + 4, 0xffffffff);
  Reject(dir + 8, U32(original, dir));
  // Unknown condition categories/strategies are preserved metadata, never
  // treated as a successful visibility result by this parser.
  auto b = original;
  Put(b, expr, 99);
  Put(b, marker + 20, 99, 1);
  Check(ParseNativeCheckpoints(b, "Field", out) &&
            out[0].markers[0].condition_strategy == 99 &&
            out[0].markers[0].conditions[0].category == 99,
        "unknown predicates remain explicit and unevaluated");
  Check(ParseNativeCheckpoints(original, "Field", out) && out == expected,
        "valid source recovers deterministically");
  std::cout << checks << " checks passed\n";
}
