// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_map.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace echoes {
namespace {
constexpr size_t NativeMapCount = 28, RecordStride = 0xd0;
constexpr size_t MaxMapKey = 63;
struct Guard { uint64_t offset; std::array<uint8_t, 16> bytes; };
// Supported original traversal bounds/registry/read routes; no game assets.
constexpr std::array<Guard, 3> Guards{{
  {0x1688dcc, {0x36,0x89,0x40,0xb9,0x48,0x03,0x00,0x54,0xdf,0xfe,0x07,0x71,0x08,0x03,0x00,0x54}},
  {0x2386de0, {0xfd,0x7b,0xba,0xa9,0xfb,0x0b,0x00,0xf9,0xfa,0x67,0x02,0xa9,0xf8,0x5f,0x03,0xa9}},
  {0x261c890, {0x7f,0x7f,0x00,0x71,0x88,0xff,0xff,0x54,0x48,0x03,0x40,0xf9,0xa9,0x7e,0x43,0xd3}},
}};
constexpr std::array<Guard, 3> LevelGuards{{
  {0x360870, {0xfd,0x7b,0xbd,0xa9,0xf5,0x0b,0x00,0xf9,0xf4,0x4f,0x02,0xa9,0xfd,0x03,0x00,0x91}},
  {0x165b4c4, {0x68,0xf2,0x00,0xd0,0x08,0xe1,0x04,0x91,0x3f,0x00,0x08,0xeb,0x40,0x01,0x00,0x54}},
  {0x16659e8, {0x08,0xe0,0x00,0xf0,0x08,0xe1,0x1c,0x91,0x00,0x27,0x80,0x52,0xc8,0x02,0x00,0xf9}},
}};
bool Add(uint64_t a, uint64_t b, uint64_t& out) {
  if (a > std::numeric_limits<uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}
bool Bytes(const ReadFn& read, uint64_t at, void* out, size_t n) {
  return at && n <= std::numeric_limits<uint64_t>::max() - at &&
         (n == 0 || read(at, out, n));
}
bool Field(const ReadFn& read, uint64_t at, uint64_t off, void* out, size_t n) {
  uint64_t address{};
  return Add(at, off, address) && Bytes(read, address, out, n);
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32;
}
bool Pointer(const ReadFn& read, uint64_t at, uint64_t off, uint64_t& out) {
  std::array<uint8_t, 8> bytes{};
  uint64_t address{};
  if (!Add(at, off, address) || address % 8 ||
      !Bytes(read, address, bytes.data(), bytes.size())) return false;
  out = Le64(bytes.data());
  return out && out % 8 == 0;
}
bool KeyCharacter(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}
bool Vtable(const ReadFn& read, uint64_t object, uint64_t base, uint64_t offset) {
  uint64_t observed{}, expected{};
  return Add(base, offset, expected) && Pointer(read, object, 0, observed) &&
         observed == expected;
}
struct Level {
  MapLevelSource source{};
  std::string key;
  bool operator==(const Level&) const = default;
};
bool ReadLevel(const ReadFn& read, uint64_t base, Level& out) {
  for (const auto& guard : LevelGuards) {
    std::array<uint8_t, 16> bytes{};
    if (!Field(read, base, guard.offset, bytes.data(), bytes.size()) || !MatchSupportedGuard(bytes, guard.bytes, guard.offset))
      return false;
  }
  auto& s = out.source;
  std::array<uint8_t, 16> name{};
  // Mirrors the original current-level accessor's owner route and RTTI class.
  // No native RTTI/accessor calls or reference-count changes are performed.
  if (!Pointer(read, base, 0x34bd718, s.controller) ||
      !Vtable(read, s.controller, base, 0x3269630) ||
      !Pointer(read, s.controller, 0x80, s.handle) ||
      !Vtable(read, s.handle, base, 0x3268098) ||
      !Pointer(read, s.handle, 0x90, s.resource) ||
      !Vtable(read, s.resource, base, 0x3268738) ||
      !Field(read, s.resource, 0x140, name.data(), name.size())) return false;
  s.key_pointer = Le64(name.data());
  const auto length = Le64(name.data() + 8);
  if (!length || length > MaxMapKey) return false;
  out.key.resize(static_cast<size_t>(length));
  return Bytes(read, s.key_pointer, out.key.data(), out.key.size()) &&
         std::all_of(out.key.begin(), out.key.end(), KeyCharacter);
}
bool StillLevel(const ReadFn& read, uint64_t base, const Level& expected) {
  uint64_t controller{}, handle{}, resource{};
  std::array<uint8_t, 16> name{};
  return Pointer(read, base, 0x34bd718, controller) &&
         controller == expected.source.controller &&
         Vtable(read, controller, base, 0x3269630) &&
         Pointer(read, controller, 0x80, handle) && handle == expected.source.handle &&
         Vtable(read, handle, base, 0x3268098) &&
         Pointer(read, handle, 0x90, resource) && resource == expected.source.resource &&
         Vtable(read, resource, base, 0x3268738) &&
         Field(read, resource, 0x140, name.data(), name.size()) &&
         Le64(name.data()) == expected.source.key_pointer &&
         Le64(name.data() + 8) == expected.key.size();
}
struct Frame {
  MapEpochSource source{};
  std::array<uint8_t, 0x30> registry_header{};
  std::array<uint8_t, NativeMapCount * 16> entries{};
  std::array<std::string, NativeMapCount> keys{};
  std::array<uint8_t, MaxTraversalMaps * RecordStride> records{};
  bool operator==(const Frame&) const = default;
};
bool Capture(const ReadFn& read, uint64_t base, const EpochSource& epoch,
             Frame& out, bool payload = true) {
  out.source.save_epoch = epoch;
  uint64_t access{}, save{};
  if (!Pointer(read, base, 0x34bc848, access) || access != epoch.access_object ||
      !Pointer(read, access, 0x10, save) || save != epoch.save_data ||
      !Pointer(read, save, 0x16b80, out.source.traversal) ||
      !Pointer(read, base, 0x34b80b8, out.source.registry_owner) ||
      !Pointer(read, out.source.registry_owner, 0x78, out.source.registry_header) ||
      !Bytes(read, out.source.registry_header, out.registry_header.data(),
             out.registry_header.size())) return false;
  if (out.registry_header[0x28] != NativeMapCount) return false;
  out.source.registry_entries = Le64(out.registry_header.data() + 0x20);
  if (!out.source.registry_entries || out.source.registry_entries % 8 ||
      !Bytes(read, out.source.registry_entries, out.entries.data(), out.entries.size()))
    return false;
  for (size_t i = 0; i < NativeMapCount; ++i) {
    const auto* entry = out.entries.data() + i * 16;
    const uint64_t pointer = Le64(entry);
    const uint32_t length = Le32(entry + 8);
    // Only native reserved slot12 may be empty. Length is u32, not pointer-sized.
    if (length > MaxMapKey || (length == 0) != (i == 12)) return false;
    if (!length) continue;
    std::string key(length, '\0');
    if (!Bytes(read, pointer, key.data(), key.size()) ||
        !std::all_of(key.begin(), key.end(), KeyCharacter)) return false;
    if (std::find(out.keys.begin(), out.keys.begin() + i, key) !=
        out.keys.begin() + i) return false;
    out.keys[i] = std::move(key);
  }
  if (out.keys[0] != "Field") return false;
  return !payload || Bytes(read, out.source.traversal, out.records.data(), out.records.size());
}
} // namespace
bool MapDiscovery::Discovered(size_t index) const {
  return index < TraversalBits && (discovered[index / 8] & (1u << (index % 8)));
}
bool MapDiscovery::Presented(size_t index) const {
  return index < TraversalBits && (presented[index / 8] & (1u << (index % 8)));
}
MapSnapshot ReadMapSnapshot(const ReadFn& read, uint64_t base, bool build_matches) {
  MapSnapshot out;
  if (!read || !build_matches || !base || base % 8 ||
      base > std::numeric_limits<uint64_t>::max() - 0x34bf100) return out;
  const auto first = ReadSnapshot(read, base, build_matches);
  if (!first.compatible) return out;
  for (const auto& guard : Guards) {
    std::array<uint8_t, 16> bytes{};
    if (!Field(read, base, guard.offset, bytes.data(), bytes.size()) || !MatchSupportedGuard(bytes, guard.bytes, guard.offset))
      return out;
  }
  out.compatible = true;
  if (!first.collection_valid) return out;
  Level level_a, level_b, level_tail;
  const bool level_observed = ReadLevel(read, base, level_a);
  Frame a, b, final_frame;
  if (!Capture(read, base, first.epoch_source, a) ||
      !Capture(read, base, first.epoch_source, b) || a != b) return out;
  const auto final = ReadSnapshot(read, base, build_matches);
  if (!final.collection_valid || first.epoch_source != final.epoch_source ||
      first.keys != final.keys ||
      !Capture(read, base, final.epoch_source, final_frame) || a != final_frame) return out;
  const bool level_stable = level_observed && ReadLevel(read, base, level_b) &&
                            level_a == level_b;
  // The final payload read can itself race a save switch. Reauthenticate the
  // save/list epoch after it, as well as the separately owned registry chain.
  Frame registry_tail;
  if (!Capture(read, base, first.epoch_source, registry_tail, false) ||
      registry_tail.source != a.source || registry_tail.registry_header != a.registry_header ||
      registry_tail.entries != a.entries || registry_tail.keys != a.keys) return out;
  const auto tail = ReadSnapshot(read, base, build_matches);
  uint64_t traversal{}, registry{}, header{};
  if (!tail.collection_valid || tail.epoch_source != first.epoch_source ||
      tail.keys != first.keys ||
      !Pointer(read, tail.epoch_source.save_data, 0x16b80, traversal) ||
      traversal != a.source.traversal ||
      !Pointer(read, base, 0x34b80b8, registry) || registry != a.source.registry_owner ||
      !Pointer(read, registry, 0x78, header) || header != a.source.registry_header)
    return out;
  const bool level_valid = level_stable && ReadLevel(read, base, level_tail) &&
                           level_tail == level_a && StillLevel(read, base, level_a);
  // A final level-name read must not authenticate discovery from another save.
  // It may have crossed a scene/save boundary even when all level pointers held.
  uint64_t access{}, save{};
  if (!Pointer(read, base, 0x34bc848, access) || access != first.epoch_source.access_object ||
      !Pointer(read, access, 0x10, save) || save != first.epoch_source.save_data)
    return out;
  if (level_stable) {
    const auto level_epoch = ReadSnapshot(read, base, build_matches);
    if (!level_epoch.collection_valid || level_epoch.epoch_source != first.epoch_source ||
        level_epoch.keys != first.keys) return out;
  }
  out.discovery_valid = true;
  out.source = a.source;
  out.map_count = NativeMapCount;
  if (level_valid) {
    out.current_level_valid = true;
    out.current_level_source = level_a.source;
    out.current_level_key = std::move(level_a.key);
  }
  for (size_t i = 0; i < NativeMapCount; ++i) {
    auto& map = out.maps[i];
    map.key = std::move(a.keys[i]);
    const auto* record = a.records.data() + i * RecordStride;
    std::copy_n(record + 0x30, map.discovered.size(), map.discovered.begin());
    std::copy_n(record + 0x70, map.presented.size(), map.presented.begin());
  }
  return out;
}
} // namespace echoes
