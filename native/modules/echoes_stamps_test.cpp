// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_stamps.h"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool value, const char* message) {
  ++checks;
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }
int64_t Tap(int32_t x, int32_t y) {
  return std::bit_cast<int64_t>((uint64_t{static_cast<uint32_t>(x)} << 32) |
                              static_cast<uint32_t>(y));
}
void Encoding() {
  StampRecord record; int64_t packed{-1};
  Check(PackStamp({}, packed) && packed == 0, "empty packs as tombstone");
  Check(UnpackStamp(0, record) && !record.present(), "tombstone restores empty");
  for (uint8_t kind = 1; kind <= 6; ++kind) {
    for (const StampUv uv : {StampUv{0, 0}, {1, 1}, {.32413663, .19664463}, {.5, .5}}) {
      Check(MakeStamp(static_cast<StampKind>(kind), uv, record), "native kind/UV accepted");
      Check(PackStamp(record, packed) && packed > 0, "present record positive");
      StampRecord round;
      Check(UnpackStamp(packed, round) && round == record, "all fields roundtrip exactly");
      Check(std::abs(round.uv().u - uv.u) <= .5 / 65535 &&
            std::abs(round.uv().v - uv.v) <= .5 / 65535, "quantization bounded to half quantum");
    }
  }
  Check(MakeStamp(StampKind::Heart, {.5, .25}, record) && PackStamp(record, packed), "malformed anchor");
  const auto valid = static_cast<uint64_t>(packed);
  for (const uint64_t invalid : {valid | (UINT64_C(1) << 40),
      valid & ~(UINT64_C(1) << 35), (valid & ~(UINT64_C(15) << 36)),
      (valid & ~(UINT64_C(15) << 36)) | (UINT64_C(2) << 36),
      valid & ~(UINT64_C(7) << 32), valid | (UINT64_C(7) << 32)}) {
    record = {StampKind::Diamond, 12, 34};
    Check(!UnpackStamp(static_cast<int64_t>(invalid), record) && record == StampRecord{},
          "reserved/version/present/type malformed refuses and clears result");
  }
  Check(!UnpackStamp(-1, record), "negative values refused");
  Check(!PackStamp({StampKind::Empty, 1, 0}, packed), "noncanonical tombstone refused");
  for (const auto kind : {StampKind::Empty, static_cast<StampKind>(7), static_cast<StampKind>(255)})
    Check(!MakeStamp(kind, {.5, .5}, record), "unknown/empty add kind refused");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const StampUv uv : {StampUv{nan, 0}, {0, inf}, {-.001, .5}, {.5, 1.001}})
    Check(!MakeStamp(StampKind::Heart, uv, record) && record == StampRecord{}, "bad UV refused");
}
void Store() {
  StampStore store;
  for (size_t i = 0; i < StampCapacity; ++i) {
    const auto at = store.Add(StampKind::Chest, {static_cast<double>(i) / 24, .25});
    Check(at && *at == i, "first free slot used deterministically");
  }
  const auto full = store.Packed();
  Check(store.count() == 24 && !store.Add(StampKind::Heart, {.5, .5}) && store.Packed() == full,
        "full refuses without eviction");
  Check(!store.Remove(24) && store.Packed() == full, "out of range remove harmless");
  Check(store.Remove(7) && !store.Remove(7), "remove is a tombstone and repeat is a miss");
  Check(store.Add(StampKind::BlueFlag, {1, 0}) == 7, "first deleted slot reused");
  const auto before = store.Packed();
  Check(!store.Add(StampKind::Empty, {.5, .5}) && store.Packed() == before, "invalid add atomic");
  auto bad = full; bad[23] = -1;
  Check(!store.Restore(bad) && store.Packed() == before, "late malformed restore leaves whole prior store");
  Check(!store.Restore(std::span<const int64_t>(full).first(23)) && store.Packed() == before,
        "short restore cannot partially overwrite");
  std::array<int64_t, 25> long_input{};
  Check(!store.Restore(long_input) && store.Packed() == before, "oversize restore refused");
  Check(store.Restore(full) && store.Packed() == full, "complete replacement accepted");
  PackedStamps empty{};
  Check(store.Restore(empty) && store.count() == 0, "complete empty restore clears");
  store.Add(StampKind::Heart, {.5, .5});
  store.Add(StampKind::Chest, {.5, .5});
  const auto exact = store.records()[0].uv();
  Check(store.RemoveNear(exact, 0) == 0, "exact duplicate tie removes lower slot");
  Check(store.RemoveNear(exact, 0) == 1, "second duplicate remains independently removable");
  store.Add(StampKind::Heart, {.5, .5});
  const auto anchor = store.Packed();
  Check(!store.RemoveNear({1, 1}, 1) && store.Packed() == anchor, "far miss unchanged");
  Check(!store.RemoveNear({.5, .5}, -1) && store.Packed() == anchor, "negative radius refused");
  Check(!store.RemoveNear({.5, .5}, std::numeric_limits<double>::infinity()) &&
        store.Packed() == anchor, "unbounded radius refused");
  store.Clear();
  store.Add(StampKind::Heart, {.51, .5}); //12.72 native pixels horizontally
  store.Add(StampKind::Chest, {.5, .512}); //10.992 native pixels vertically
  Check(store.RemoveNear({.5, .5}, 12) == 1, "native Field aspect used instead of UV distance");
  Check(!store.RemoveNear({.5, .5}, 12), "native horizontal point outside radius retained");
  Check(store.count() == 1, "near removal removes only one nearest record");
}
void ViewTransforms() {
  const StampImageView view{912, 657, 2, 100, 80};
  StampUv uv; StampPoint point;
  Check(StampTapUv(Tap(456, 328), view, uv), "tap unpacked in widget coordinates");
  Check(Near(uv.u, (100 + 456.0 / 2) / 912) &&
        Near(uv.v, (80 + 328.0 / 2) / 657), "pan plus inverse zoom matches runtime source window");
  Check(StampViewPoint(uv, view, point) && Near(point.x, 456) && Near(point.y, 328),
        "forward transform returns same pixel");
  for (const double zoom : {1.0, 1.25, 2.24, 4.0}) {
    for (const double pan : {-100.0, 0.0, 80.0, 99999.0}) {
      const StampImageView varied{600, 800, zoom, pan, pan};
      for (const StampPoint tap : {StampPoint{0, 0}, {170, 510}, {599, 799}}) {
        Check(StampTapUv(Tap(static_cast<int32_t>(tap.x), static_cast<int32_t>(tap.y)), varied, uv),
              "finite 3:4 image tap accepted with clamped pan");
        Check(StampViewPoint(uv, varied, point) && std::abs(point.x - tap.x) < 1e-8 &&
              std::abs(point.y - tap.y) < 1e-8, "roundtrip at fractional zoom and edge pan");
      }
    }
  }
  const StampImageView clipped{600, 800, 2, 150, 200};
  Check(!StampViewPoint({0, 0}, clipped, point), "off-viewport stamp hidden, not clamped to edge");
  for (const auto tap : {Tap(-1, 0), Tap(0, -1), Tap(912, 0), Tap(0, 657)})
    Check(!StampTapUv(tap, view, uv) && uv.u == 0 && uv.v == 0, "outside local tap refused");
  for (const StampImageView invalid : {StampImageView{0, 600, 1, 0, 0},
       {600, 800, .5, 0, 0}, {600, 800, 5, 0, 0}, {9000, 800, 1, 0, 0},
       {600, 800, 2, std::numeric_limits<double>::quiet_NaN(), 0}})
    Check(!StampTapUv(Tap(1, 1), invalid, uv), "invalid view refused");
}
} // namespace
int main() {
  Encoding(); Store(); ViewTransforms();
  std::cout << checks << " stamp checks passed\n";
}
