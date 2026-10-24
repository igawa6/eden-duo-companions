// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_stamps.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace echoes {
namespace {
constexpr uint64_t Present = UINT64_C(1) << 35;
constexpr uint64_t Version = UINT64_C(1) << 36;
constexpr uint64_t Allowed = (UINT64_C(1) << 40) - 1;
bool Kind(StampKind kind) {
  return kind >= StampKind::RedFlag && kind <= StampKind::Diamond;
}
bool Uv(StampUv uv) {
  return std::isfinite(uv.u) && std::isfinite(uv.v) &&
      uv.u >= 0 && uv.u <= 1 && uv.v >= 0 && uv.v <= 1;
}
bool View(const StampImageView& view, double& px, double& py) {
  if (!std::isfinite(view.width) || !std::isfinite(view.height) ||
      !std::isfinite(view.zoom) || !std::isfinite(view.pan_x) || !std::isfinite(view.pan_y) ||
      view.width < 1 || view.width > 8192 || view.height < 1 || view.height > 8192 ||
      view.zoom < 1 || view.zoom > 4) return false;
  px = std::clamp(view.pan_x, 0.0, view.width * (1 - 1 / view.zoom));
  py = std::clamp(view.pan_y, 0.0, view.height * (1 - 1 / view.zoom));
  return true;
}
} // namespace
StampUv StampRecord::uv() const {
  return {static_cast<double>(u_q16) / 65535, static_cast<double>(v_q16) / 65535};
}
bool MakeStamp(StampKind kind, StampUv uv, StampRecord& out) {
  out = {};
  if (!Kind(kind) || !Uv(uv)) return false;
  out = {kind, static_cast<uint16_t>(std::lround(uv.u * 65535)),
               static_cast<uint16_t>(std::lround(uv.v * 65535))};
  return true;
}
bool PackStamp(const StampRecord& record, int64_t& out) {
  out = 0;
  if (record.kind == StampKind::Empty)
    return record.u_q16 == 0 && record.v_q16 == 0;
  if (!Kind(record.kind)) return false;
  const uint64_t value = Version | Present | (static_cast<uint64_t>(record.kind) << 32) |
      (static_cast<uint64_t>(record.v_q16) << 16) | record.u_q16;
  out = static_cast<int64_t>(value);
  return true;
}
bool UnpackStamp(int64_t packed, StampRecord& out) {
  out = {};
  if (!packed) return true;
  if (packed < 0) return false;
  const auto value = static_cast<uint64_t>(packed);
  if ((value & ~Allowed) || !(value & Present) || ((value >> 36) & 15) != 1) return false;
  const auto kind = static_cast<StampKind>((value >> 32) & 7);
  if (!Kind(kind)) return false;
  out = {kind, static_cast<uint16_t>(value & 65535),
               static_cast<uint16_t>((value >> 16) & 65535)};
  return true;
}
size_t StampStore::count() const {
  return static_cast<size_t>(std::count_if(records_.begin(), records_.end(),
      [](const StampRecord& record) { return record.present(); }));
}
bool StampStore::Restore(std::span<const int64_t> packed) {
  if (packed.size() != StampCapacity) return false;
  StampRecords restored{};
  for (size_t i = 0; i < StampCapacity; ++i)
    if (!UnpackStamp(packed[i], restored[i])) return false;
  records_ = restored;
  return true;
}
PackedStamps StampStore::Packed() const {
  PackedStamps packed{};
  for (size_t i = 0; i < StampCapacity; ++i) PackStamp(records_[i], packed[i]);
  return packed;
}
std::optional<size_t> StampStore::Add(StampKind kind, StampUv uv) {
  StampRecord record;
  if (!MakeStamp(kind, uv, record)) return {};
  for (size_t i = 0; i < StampCapacity; ++i) {
    if (!records_[i].present()) { records_[i] = record; return i; }
  }
  return {};
}
bool StampStore::Remove(size_t index) {
  if (index >= StampCapacity || !records_[index].present()) return false;
  records_[index] = {};
  return true;
}
std::optional<size_t> StampStore::RemoveNear(StampUv uv, double radius) {
  if (!Uv(uv) || !std::isfinite(radius) || radius < 0 ||
      radius > std::hypot(StampFieldWidth, StampFieldHeight)) return {};
  std::optional<size_t> best;
  double best_distance = radius * radius;
  for (size_t i = 0; i < StampCapacity; ++i) {
    if (!records_[i].present()) continue;
    const auto at = records_[i].uv();
    const double dx = (at.u - uv.u) * StampFieldWidth;
    const double dy = (at.v - uv.v) * StampFieldHeight;
    const double distance = dx * dx + dy * dy;
    if (distance <= best_distance && (!best || distance < best_distance)) {
      best = i; best_distance = distance;
    }
  }
  if (best) records_[*best] = {};
  return best;
}
bool StampTapUv(int64_t packed, const StampImageView& view, StampUv& out) {
  out = {};
  double px{}, py{};
  if (!View(view, px, py)) return false;
  const auto bits = std::bit_cast<uint64_t>(packed);
  const double x = std::bit_cast<int32_t>(static_cast<uint32_t>(bits >> 32));
  const double y = std::bit_cast<int32_t>(static_cast<uint32_t>(bits));
  if (x < 0 || y < 0 || x >= view.width || y >= view.height) return false;
  out = {(px + x / view.zoom) / view.width, (py + y / view.zoom) / view.height};
  return Uv(out);
}
bool StampViewPoint(StampUv uv, const StampImageView& view, StampPoint& out) {
  out = {};
  double px{}, py{};
  if (!Uv(uv) || !View(view, px, py)) return false;
  const double x = view.zoom * (uv.u * view.width - px);
  const double y = view.zoom * (uv.v * view.height - py);
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
      x >= view.width || y >= view.height) return false;
  out = {x, y};
  return true;
}
} // namespace echoes
