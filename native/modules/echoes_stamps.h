// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace echoes {
inline constexpr size_t StampCapacity = 24;
inline constexpr double StampFieldWidth = 1272.0;
inline constexpr double StampFieldHeight = 916.0;
// Native IconMapPin roles; zero is the empty/tombstone state.
enum class StampKind : uint8_t {
  Empty, RedFlag, BlueFlag, GreenFlag, Heart, Chest, Diamond
};
struct StampUv { double u{}, v{}; };
struct StampRecord {
  StampKind kind{StampKind::Empty};
  uint16_t u_q16{}, v_q16{};
  bool present() const { return kind != StampKind::Empty; }
  StampUv uv() const;
  bool operator==(const StampRecord&) const = default;
};
using StampRecords = std::array<StampRecord, StampCapacity>;
using PackedStamps = std::array<int64_t, StampCapacity>;
// A record is a positive int64: U[0:15], V[16:31], kind[32:34],
// present[35], version=1[36:39]. All other bits must be zero. Tombstone is zero.
// Invalid input is rejected, never clamped or silently migrated.
bool MakeStamp(StampKind, StampUv, StampRecord&);
bool PackStamp(const StampRecord&, int64_t&);
bool UnpackStamp(int64_t, StampRecord&);

// Pure bounded companion-local state. No game memory, persistence, save-slot
// identity or file I/O. Runtime persist_flags are profile-global unless the
// caller explicitly supplies a stronger namespace; callers own all scope gates.
class StampStore {
public:
  const StampRecords& records() const { return records_; }
  size_t count() const;
  bool Restore(std::span<const int64_t>); // exactly24, all-or-nothing validation
  PackedStamps Packed() const;
  std::optional<size_t> Add(StampKind, StampUv);
  bool Remove(size_t);
  // Nearest existing record in native1272x916 terrain-pixel distance. Ties use
  // the lower slot. An invalid query or a miss leaves every record unchanged.
  std::optional<size_t> RemoveNear(StampUv, double radius_native_pixels);
  void Clear() { records_ = {}; }
private:
  StampRecords records_{};
};

// Existing runtime22 full-source pan_zoom IMAGE convention (not a Map widget).
// Pan is measured in unzoomed logical widget pixels. No src_rect/pan_bounds
// or padded-source-to-Field transform is inferred here; callers apply those.
struct StampImageView {
  double width{}, height{}, zoom{1}, pan_x{}, pan_y{};
};
struct StampPoint { double x{}, y{}; };
// payload="$tap": signed widget-local x in high32, y in low32. Runtime does
// not unproject an image tap. Finite bounded views only; pan clamps like host.
bool StampTapUv(int64_t packed_tap, const StampImageView&, StampUv&);
bool StampViewPoint(StampUv, const StampImageView&, StampPoint&);
} // namespace echoes
