// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_reader.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace echoes {
namespace {
constexpr uint64_t AccessOffset = 0x34bc848;
constexpr uint64_t SavePointerOffset = 0x10;
constexpr uint64_t CollectionPointerOffset = 0x16cf8;
constexpr size_t HeaderSize = 0x20;

bool Add(uint64_t base, uint64_t offset, uint64_t& result) {
  if (base > std::numeric_limits<uint64_t>::max() - offset)
    return false;
  result = base + offset;
  return true;
}
bool Bytes(const ReadFn& read, uint64_t at, void* out, size_t size) {
  if (!at || size > std::numeric_limits<uint64_t>::max() - at)
    return false;
  return size == 0 || read(at, out, size);
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32;
}
bool Pointer(const ReadFn& read, uint64_t base, uint64_t offset, uint64_t& out) {
  uint64_t at{};
  std::array<uint8_t, 8> bytes{};
  if (!Add(base, offset, at) || at % 8 ||
      !Bytes(read, at, bytes.data(), bytes.size()))
    return false;
  out = Le64(bytes.data());
  return out != 0 && out % 8 == 0;
}
struct Collection {
  EpochSource source;
  std::array<uint8_t, HeaderSize> header{};
  std::vector<uint32_t> keys;
  bool operator==(const Collection&) const = default;
};
bool Collect(const ReadFn& read, uint64_t base, Collection& out) {
  auto& source = out.source;
  if (!Pointer(read, base, AccessOffset, source.access_object) ||
      !Pointer(read, source.access_object, SavePointerOffset, source.save_data) ||
      !Pointer(read, source.save_data, CollectionPointerOffset, source.list) ||
      !Bytes(read, source.list, out.header.data(), out.header.size()))
    return false;
  const uint32_t count = Le32(out.header.data() + 0x10);
  source.revision = Le32(out.header.data() + 0x18);
  if (count > MaxLearnedKeys)
    return false;
  uint64_t at{};
  if (!Add(source.list, HeaderSize, at))
    return false;
  std::array<uint8_t, MaxLearnedKeys * 4> bytes{};
  if (!Bytes(read, at, bytes.data(), size_t(count) * 4))
    return false;
  out.keys.reserve(count);
  for (size_t i = 0; i < count; ++i)
    out.keys.push_back(Le32(bytes.data() + i * 4));
  auto sorted = out.keys;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}
bool StillCurrent(const ReadFn& read, uint64_t base, const Collection& expected) {
  std::array<uint8_t, HeaderSize> header{};
  EpochSource source;
  // Validate metadata and the entire owner chain after the second key read.
  // A save switch during that read must not authenticate a detached old list.
  return Bytes(read, expected.source.list, header.data(), header.size()) &&
         header == expected.header &&
         Pointer(read, base, AccessOffset, source.access_object) &&
         source.access_object == expected.source.access_object &&
         Pointer(read, source.access_object, SavePointerOffset, source.save_data) &&
         source.save_data == expected.source.save_data &&
         Pointer(read, source.save_data, CollectionPointerOffset, source.list) &&
         source.list == expected.source.list;
}
struct Favorites {
  uint64_t data{};
  std::array<uint8_t, 16> header{};
  uint32_t count{};
  std::vector<uint32_t> keys;
  bool operator==(const Favorites&) const = default;
};
bool ReadFavorites(const ReadFn& read, uint64_t base,
                   const Collection& collection, Favorites& out) {
  uint64_t count_at{}, keys_at{};
  std::array<uint8_t, 4> count{};
  if (!StillCurrent(read, base, collection) ||
      !Pointer(read, collection.source.save_data, 0x16d60, out.data) ||
      !Bytes(read, out.data, out.header.data(), out.header.size()) ||
      !Add(out.data, 0x3058, count_at) ||
      !Bytes(read, count_at, count.data(), count.size()))
    return false;
  out.count = Le32(count.data());
  // Native storage supports1024; every exposed favorite must additionally be
  // learned in this exact bounded collection. Never publish unknown CRCs.
  if (out.count > 1024 || out.count > collection.keys.size() ||
      !Add(out.data, 0x2058, keys_at))
    return false;
  std::array<uint8_t, MaxLearnedKeys * 4> bytes{};
  if (!Bytes(read, keys_at, bytes.data(), size_t(out.count) * 4))
    return false;
  for (size_t i = 0; i < out.count; ++i) {
    const auto key = Le32(bytes.data() + i * 4);
    if (std::find(collection.keys.begin(), collection.keys.end(), key) ==
        collection.keys.end())
      return false;
    out.keys.push_back(key);
  }
  auto sorted = out.keys;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}
bool FavoriteGuards(const ReadFn& read, uint64_t base) {
  for (const auto& guard : FavoriteIdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> observed{};
    if (!Add(base, guard.offset, at) ||
        !Bytes(read, at, observed.data(), observed.size()) || !MatchSupportedGuard(observed, guard.bytes, guard.offset))
      return false;
  }
  return true;
}
} // namespace

bool SupportsBuild(std::string_view build) {
  if (build.size() != BuildId.size())
    return false;
  for (size_t i = 0; i < build.size(); ++i) {
    const char c = build[i] >= 'a' && build[i] <= 'f'
                       ? char(build[i] - 'a' + 'A') : build[i];
    if (c != BuildId[i])
      return false;
  }
  return true;
}

Snapshot ReadSnapshot(const ReadFn& read, uint64_t base, bool build_matches) {
  Snapshot out;
  if (!read || !build_matches || !base || base % 4)
    return out;
  for (const auto& guard : IdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> observed{};
    if (!Add(base, guard.offset, at) ||
        !Bytes(read, at, observed.data(), observed.size()) || !MatchSupportedGuard(observed, guard.bytes, guard.offset))
      return out;
  }
  out.compatible = true;
  Collection first, second;
  if (!Collect(read, base, first) || !Collect(read, base, second) || first != second ||
      !StillCurrent(read, base, first))
    return out;
  Favorites favorite_first, favorite_second, favorite_third;
  bool favorite_valid =
      FavoriteGuards(read, base) &&
      ReadFavorites(read, base, first, favorite_first) &&
      ReadFavorites(read, base, first, favorite_second) &&
      favorite_first == favorite_second &&
      ReadFavorites(read, base, first, favorite_third) &&
      favorite_first == favorite_third;
  uint64_t favorite_data{};
  std::array<uint8_t, 4> final_count{};
  uint64_t count_at{};
  favorite_valid = favorite_valid &&
      Pointer(read, first.source.save_data, 0x16d60, favorite_data) &&
      favorite_data == favorite_first.data && Add(favorite_data, 0x3058, count_at) &&
      Bytes(read, count_at, final_count.data(), final_count.size()) &&
      Le32(final_count.data()) == favorite_first.count;
  if (!StillCurrent(read, base, first))
    return out;
  out.collection_valid = true;
  out.epoch_source = first.source;
  out.keys = std::move(first.keys);
  if (favorite_valid) {
    out.favorite_valid = true;
    out.favorite_crc_keys = std::move(favorite_first.keys);
  }
  return out;
}
} // namespace echoes
