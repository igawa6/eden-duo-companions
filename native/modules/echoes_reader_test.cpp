// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic unmapped/torn-memory adversaries; no gameplay-context claim.
#include "echoes_reader.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x80488000, Access = 0x2107058df8,
                   Save = 0x2107095000, List = 0x21070a0ee0,
                   FavoriteData = 0x2107300000;
size_t cases{};
void Check(bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    std::exit(1);
  }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> memory;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i)
      memory[at + i] = uint8_t(value >> (8 * i));
  }
  explicit Memory(size_t count = 3) {
    for (const auto& guard : echoes::IdentityGuards)
      for (size_t i = 0; i < guard.bytes.size(); ++i)
        memory[Base + guard.offset + i] = guard.bytes[i];
    // Literal fixture offsets are independent of the reader's private constants.
    Put(Base + 0x34bc848, Access, 8);
    Put(Access + 0x10, Save, 8);
    Put(Save + 0x16cf8, List, 8);
    for (size_t i = 0; i < 0x20; ++i)
      memory[List + i] = 0;
    Put(List + 0x10, count, 4);
    Put(List + 0x18, 0x8c1b4ca5, 4);
    for (size_t i = 0; i < count; ++i)
      Put(List + 0x20 + i * 4, 0x10203040 + i, 4);
  }
  bool Read(uint64_t at, void* out, size_t size) {
    ++reads;
    largest = std::max(largest, size);
    const size_t visit = ++visits[at];
    if (before)
      before(at, size, visit);
    if (!at || size > std::numeric_limits<uint64_t>::max() - at)
      return false;
    auto* target = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < size; ++i) {
      auto p = memory.find(at + i);
      if (p == memory.end())
        return false;
      target[i] = p->second;
    }
    return true;
  }
  void Favorites(std::vector<uint32_t> keys = {0x10203041}) {
    for (const auto& guard : echoes::FavoriteIdentityGuards)
      for (size_t i = 0; i < guard.bytes.size(); ++i)
        memory[Base + guard.offset + i] = guard.bytes[i];
    Put(Save + 0x16d60, FavoriteData, 8);
    for (size_t i = 0; i < 16; ++i)
      memory[FavoriteData + i] = 0;
    Put(FavoriteData + 0x3058, keys.size(), 4);
    for (size_t i = 0; i < keys.size(); ++i)
      Put(FavoriteData + 0x2058 + i * 4, keys[i], 4);
  }
  echoes::Snapshot Sample(bool build = true, uint64_t base = Base) {
    return echoes::ReadSnapshot([&](auto a, auto p, auto n) { return Read(a, p, n); },
                                base, build);
  }
};
void Unavailable(const echoes::Snapshot& s) {
  Check(s.compatible && !s.collection_valid && s.keys.empty() &&
            s.epoch_source == echoes::EpochSource{} &&
            s.context == echoes::Context::Unknown,
        "invalid collection clears all data without inventing context");
}
} // namespace

int main() {
  {
    Memory m;
    auto s = m.Sample();
    Check(s.compatible && s.collection_valid &&
              s.keys == std::vector<uint32_t>{0x10203040, 0x10203041, 0x10203042},
          "stable collection preserves raw keys and order");
    Check(!s.favorite_valid && s.favorite_crc_keys.empty(),
          "missing optional favorites never becomes a valid empty set");
    Check(s.context == echoes::Context::Unknown && s.epoch_source.list == List &&
              s.epoch_source.access_object == Access && s.epoch_source.save_data == Save &&
              s.epoch_source.revision == 0x8c1b4ca5,
          "source identity is explicit and collection does not establish gameplay");
    m.Put(Base + 0x34bc848, 0, 8);
    Unavailable(m.Sample());
    ++cases;
  }
  {
    Memory m;
    m.Favorites();
    const auto s = m.Sample();
    Check(s.collection_valid && s.favorite_valid &&
              s.favorite_crc_keys == std::vector<uint32_t>{0x10203041},
          "persisted favorites expose learned CRC membership on the same save");
    ++cases;
  }
  {
    Memory m(0);
    m.Favorites({});
    const auto s = m.Sample();
    Check(s.collection_valid && s.favorite_valid && s.favorite_crc_keys.empty() &&
              m.visits[FavoriteData + 0x2058] == 0,
          "valid empty favorites differs from unknown and reads no payload");
    ++cases;
  }
  for (int fault = 0; fault < 7; ++fault) {
    Memory m;
    m.Favorites();
    if (fault == 0) m.Put(FavoriteData + 0x3058, 1025, 4);
    if (fault == 1) m.Put(FavoriteData + 0x2058, 0x99999999, 4);
    if (fault == 2) m.Favorites({0x10203041, 0x10203041});
    if (fault == 3) m.memory.erase(FavoriteData + 0x2059);
    if (fault == 4) m.Put(Save + 0x16d60, FavoriteData + 1, 8);
    if (fault == 5) m.memory[Base + echoes::FavoriteIdentityGuards[0].offset] ^= 1;
    if (fault == 6) m.Put(FavoriteData + 0x3058, 4, 4);
    const auto s = m.Sample();
    Check(s.compatible && s.collection_valid && !s.favorite_valid &&
              s.favorite_crc_keys.empty() && s.keys.size() == 3,
          "bad optional favorite count/key/duplicate/range/pointer/guard preserves collection");
    ++cases;
  }
  for (int fault = 0; fault < 4; ++fault) {
    Memory m;
    m.Favorites();
    m.before = [&](auto at, auto, auto visit) {
      if (at == FavoriteData + 0x2058 && visit == 2) {
        if (fault == 0) m.Put(FavoriteData + 0x2058, 0x10203042, 4);
        if (fault == 1) m.Put(FavoriteData + 0x3058, 0, 4);
        if (fault == 2) m.Put(Save + 0x16d60, 0, 8);
        if (fault == 3) m.Put(Base + 0x34bc848, 0, 8);
      }
    };
    const auto s = m.Sample();
    Check(!s.favorite_valid && s.favorite_crc_keys.empty() &&
              (fault == 3 ? !s.collection_valid : s.collection_valid),
          "favorite payload/count/pointer race withdraws favorites; save race withdraws collection");
    ++cases;
  }
  {
    Memory m(0);
    auto s = m.Sample();
    Check(s.compatible && s.collection_valid && s.keys.empty() &&
              s.context == echoes::Context::Unknown,
          "stable title-screen empty collection is not gameplay readiness");
    Check(m.visits[List + 0x20] == 0, "empty collection performs no payload read");
    ++cases;
  }
  {
    Memory m(512);
    Check(m.Sample().keys.size() == 512 && m.largest == 2048,
          "valid maximum stays bounded");
    ++cases;
  }
  {
    Memory m(513);
    Unavailable(m.Sample());
    Check(m.largest <= 256 && m.visits[List + 0x20] == 0,
          "oversize count rejected before payload read/allocation");
    ++cases;
  }
  {
    Memory m;
    Check(!m.Sample(false).compatible && m.reads == 0,
          "wrong build gates all reads");
    Check(!m.Sample(true, 0).compatible && m.reads == 0,
          "null base gates all reads");
    Check(!m.Sample(true, std::numeric_limits<uint64_t>::max() - 3).compatible &&
              m.reads == 0,
          "base address addition overflow gates all reads");
    Check(!echoes::ReadSnapshot({}, Base, true).compatible, "empty reader rejected");
    ++cases;
  }
  {
    std::string lower(echoes::BuildId);
    for (char& c : lower)
      if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
    Check(echoes::SupportsBuild(lower) && echoes::SupportsBuild(echoes::BuildId) &&
              !echoes::SupportsBuild(lower.substr(0, 16)),
          "full build identity accepts hex case, rejects prefixes");
    lower[0] = '0';
    Check(!echoes::SupportsBuild(lower), "different full build rejected");
    ++cases;
  }
  for (const auto& guard : echoes::IdentityGuards) {
    Memory m;
    m.memory[Base + guard.offset + 255] ^= 1;
    auto s = m.Sample();
    Check(!s.compatible && !s.collection_valid && s.keys.empty(),
          "last-byte corruption in each immutable guard refuses build");
    ++cases;
  }
  {
    Memory m;
    m.memory.erase(Base + echoes::IdentityGuards[1].offset + 120);
    Check(!m.Sample().compatible, "partly unmapped instruction guard refused");
    ++cases;
  }
  for (uint64_t pointer : {uint64_t(0), uint64_t(Access + 1),
                           std::numeric_limits<uint64_t>::max() - 7}) {
    Memory m;
    m.Put(Base + 0x34bc848, pointer, 8);
    Unavailable(m.Sample());
    ++cases;
  }
  {
    Memory m;
    m.memory.erase(List + 0x25);
    Unavailable(m.Sample());
    ++cases;
  }
  {
    Memory m;
    m.Put(List + 0x24, 0x10203040, 4);
    Unavailable(m.Sample());
    ++cases;
  }
  for (uint64_t field : {uint64_t(0x10), uint64_t(0x18)}) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == List && visit == 2) m.Put(List + field, 2, 4);
    };
    Unavailable(m.Sample());
    ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == List + 0x20 && visit == 2) m.Put(List + 0x24, 0x99887766, 4);
    };
    Unavailable(m.Sample());
    ++cases;
  }
  for (uint64_t field : {Base + 0x34bc848, Access + 0x10, Save + 0x16cf8}) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == field && visit == 2) m.Put(field, 0, 8);
    };
    Unavailable(m.Sample());
    ++cases;
  }
  // A detached old list can remain readable. Metadata changes on the second
  // payload read must still fail after both independently assembled copies agree.
  for (bool tear_owner : {false, true}) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == List + 0x20 && visit == 2) {
        if (tear_owner) m.Put(Base + 0x34bc848, 0, 8);
        else m.Put(List + 0x10, 0, 4);
      }
    };
    Unavailable(m.Sample());
    ++cases;
  }
  std::cout << cases << " Echoes reader adversarial cases passed\n";
}
