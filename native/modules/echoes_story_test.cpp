// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_story.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
using namespace echoes;
namespace {
constexpr uint64_t Base = 0x80000000, R = 0x2100000000, S = R + 0x10000,
                   Payload = S + 0x20000, List = S + 0x30000;
constexpr uint64_t Buckets = S + 0x40000, Node = S + 0x50000,
                   Stages = S + 0x60000, StageBuckets = S + 0x70000,
                   A = S + 0x80000, B = A + 32;
struct Memory {
  std::map<uint64_t, uint8_t> m;
  size_t reads{};
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  MapEpochSource source;
  StoryQuery query;
  void put(uint64_t at, uint64_t v, size_t n = 8) {
    for (size_t i = 0; i < n; ++i)
      m[at + i] = uint8_t(v >> (i * 8));
  }
  void bytes(uint64_t at, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      m[at + i] = p[i];
  }
  void hash(uint64_t at, uint64_t buckets, uint32_t count) {
    for (size_t i = 0; i < 40; ++i)
      m[at + i] = 0;
    put(at, buckets);
    put(at + 8, 1);
    put(at + 24, count);
    put(at + 32, 0x3f800000, 4);
  }
  Memory() {
    for (auto &g : IdentityGuards)
      bytes(Base + g.offset, g.bytes.data(), 256);
    for (auto &g : FieldStoryIdentityGuards)
      bytes(Base + g.offset, g.bytes.data(), 256);
    query.valid = true;
    query.quest_count = 107;
    query.stage_count = 20;
    query.quest_crc = 0x123;
    query.quest_index = 19;
    query.target_crc = 7;
    query.target_ordinal = 6;
    for (uint32_t i = 1; i <= 20; ++i)
      query.progress_crcs.push_back(i);
    source.save_epoch = {R, S, List, 3};
    source.traversal = S + 0x100000;
    source.registry_owner = S + 0x110000;
    source.registry_header = S + 0x120000;
    source.registry_entries = S + 0x130000;
    put(Base + 0x34bc848, R);
    put(R + 16, S);
    put(S + 0x16b80, source.traversal);
    put(S + 0x16cf8, List);
    for (size_t i = 0; i < 32; ++i)
      m[List + i] = 0;
    put(List + 16, 127, 4);
    put(List + 24, 3, 4);
    put(Base + 0x34b80b8, source.registry_owner);
    put(source.registry_owner + 0x78, source.registry_header);
    put(source.registry_header + 32, source.registry_entries);
    for (size_t i = 0; i < 32; ++i)
      m[S + 0x16d38 + i] = 0;
    put(S + 0x16d38, Base + 0x33ff808);
    put(Base + 0x33ff828, Base + 0x232a9b0);
    put(S + 0x16d40, 0x7777);
    put(S + 0x16d48, Payload);
    put(S + 0x16d50, 256, 4);
    for (size_t i = 0; i < 436; ++i)
      m[Payload + i] = 0;
    put(Payload, 107, 4);
    put(Payload + 4, 0x7777, 4);
    put(Payload + 8 + 19 * 4, 14, 4);
    put(Base + 0x34bc3f8, 1, 1);
    put(Base + 0x34bc390, S + 0x140000);
    hash(Base + 0x34bc3a0, Buckets, 107);
    put(Buckets, Base + 0x34bc3b0);
    put(Base + 0x34bc3b0, Node);
    put(Node, 0);
    put(Node + 8, query.quest_crc);
    put(Node + 16, query.quest_crc, 4);
    put(Node + 20, 19, 4);
    put(Base + 0x34bc3c8, Stages);
    put(Base + 0x34bc3d0, Stages + 107 * 40);
    put(Base + 0x34bc3d8, Stages + 128 * 40);
    hash(Stages + 19 * 40, StageBuckets, 20);
    put(StageBuckets, Stages + 19 * 40 + 16);
    put(Stages + 19 * 40 + 16, A);
    put(A, B);
    put(A + 8, 7);
    put(A + 16, 7, 4);
    put(A + 20, 6, 4);
    put(B, 0);
    put(B + 8, 14);
    put(B + 16, 14, 4);
    put(B + 20, 13, 4);
  }
  bool read(uint64_t a, void *p, size_t n) {
    ++reads;
    auto visit = ++visits[a];
    if (before)
      before(a, visit);
    auto out = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
      auto x = m.find(a + i);
      if (x == m.end())
        return false;
      out[i] = x->second;
    }
    return true;
  }
  FieldStorySnapshot run(bool build = true) {
    return ReadFieldStory(
        [&](uint64_t a, void *p, size_t n) { return read(a, p, n); }, Base,
        build, source, query);
  }
};
constexpr uint64_t Manager = S + 0x160000, Scene = S + 0x170000,
                   Typed = S + 0x180000, Weather = S + 0x190000;
void weather(Memory &m, uint32_t state = 0) {
  for (auto &g : SandstormIdentityGuards)
    m.bytes(Base + g.offset, g.bytes.data(), 256);
  m.put(Base + 0x34bd8e8, 1, 1);
  m.put(Base + 0x34a9140, 1, 1);
  m.put(Base + 0x34a9138, Base + 0x326a030);
  m.put(Base + 0x34bd718, Manager);
  m.put(Manager + 0x80, Scene);
  m.put(Scene, Base + 0x3268098);
  m.put(Base + 0x32680c0, Base + 0x165b4a8);
  m.put(Scene + 0xa0, Typed);
  m.put(Scene + 0xa8, Typed + 0x10000);
  m.put(Typed + 0x140, Weather);
  m.put(Weather, Base + 0x329fcd0);
  m.put(Base + 0x329fcd0, Base + 0x17a26c0);
  m.put(Base + 0x329fcd8, Base + 0x17a2720);
  m.put(Weather + 0x20, state, 4);
}
void unknown(Memory &m) {
  auto s = m.run();
  assert(!s.known && s.native_field_extension_visible &&
         s.map_source == MapEpochSource{});
}
} // namespace
int main() {
  size_t cases = 0;
  for (uint32_t state = 0; state <= 6; ++state) {
    Memory m;
    weather(m, state);
    auto x = m.run();
    assert(x.known && x.sandstorm_known && x.sandstorm_visible == (state == 5));
    assert(x.map_source == m.source);
    ++cases;
  }
  for (auto at : {Base + 0x34bd8e8, Base + 0x34a9140,
                  Base + 0x34a9138, Base + 0x34bd718,
                  Manager + 0x80, Scene, Base + 0x32680c0,
                  Scene + 0xa0, Scene + 0xa8, Typed + 0x140,
                  Weather, Base + 0x329fcd0, Base + 0x329fcd8}) {
    Memory m;
    weather(m);
    m.put(at, 0);
    auto x = m.run();
    assert(x.known && !x.sandstorm_known && x.sandstorm_visible);
    ++cases;
  }
  for (uint32_t state : {7u, UINT32_MAX}) {
    Memory m;
    weather(m, state);
    auto x = m.run();
    assert(x.known && !x.sandstorm_known && x.sandstorm_visible);
    ++cases;
  }
  for (auto &guard : SandstormIdentityGuards) {
    Memory m;
    weather(m);
    m.m[Base + guard.offset] ^= 1;
    auto x = m.run();
    assert(x.known && !x.sandstorm_known && x.sandstorm_visible);
    ++cases;
  }
  for (auto at : {Scene + 0xa0, Typed + 0x140, Weather + 0x20,
                  Base + 0x34bd718, Base + 0x34bc848}) {
    Memory m;
    weather(m);
    m.before = [&](uint64_t a, size_t visit) {
      if (a == at && visit == 2) m.put(at, at == Weather + 0x20 ? 5 : 0);
    };
    auto x = m.run();
    assert(!x.sandstorm_known && x.sandstorm_visible);
    ++cases;
  }
  {
    Memory m;
    weather(m);
    m.put(Payload + 8 + 19 * 4, UINT32_MAX, 4);
    auto x = m.run();
    assert(!x.known && x.native_field_extension_visible &&
           x.sandstorm_known && !x.sandstorm_visible);
    ++cases;
  }
  {
    Memory m;
    weather(m);
    ++m.source.save_epoch.revision;
    auto x = m.run();
    assert(!x.known && !x.sandstorm_known && x.sandstorm_visible);
    ++cases;
  }
  {
    Memory m;
    auto x = m.run();
    assert(x.known && !x.native_field_extension_visible &&
           x.current_ordinal == 13 && x.target_ordinal == 6);
    ++cases;
  }
  {
    Memory m;
    m.put(Payload + 8 + 19 * 4, 7, 4);
    auto x = m.run();
    assert(x.known && x.native_field_extension_visible);
    ++cases;
  }
  {
    Memory m;
    auto x = m.run(false);
    assert(!x.known && m.reads == 0);
    ++cases;
  }
  for (auto off : {uint64_t(0x398130), uint64_t(0x232a9b0)}) {
    Memory m;
    m.m[Base + off] ^= 1;
    unknown(m);
    ++cases;
  }
  for (auto pair : {std::pair<uint64_t, uint64_t>{Base + 0x34bc848, 0},
                    {R + 16, 0},
                    {S + 0x16b80, 0},
                    {S + 0x16cf8, 0},
                    {List + 24, 9},
                    {S + 0x16d38, 0},
                    {Base + 0x33ff828, 0},
                    {S + 0x16d48, 0},
                    {S + 0x16d50, 106},
                    {S + 0x16d50, 257},
                    {Payload, 108},
                    {Payload + 4, 8},
                    {Base + 0x34bc3f8, 0},
                    {Base + 0x34bc390, 0},
                    {Base + 0x34bc3a8, 4097},
                    {Base + 0x34bc3b8, 108},
                    {Base + 0x34bc3d0, Stages + 106 * 40},
                    {Base + 0x34bc3d8, Stages + 129 * 40},
                    {Node + 20, 18},
                    {A + 20, 5},
                    {B + 20, 12},
                    {Payload + 8 + 19 * 4, 0},
                    {Payload + 8 + 19 * 4, UINT32_MAX}}) {
    Memory m;
    m.put(pair.first, pair.second,
          pair.first == Base + 0x34bc3f8 ? 1
          : pair.first == Payload || pair.first == Payload + 4 ||
                  pair.first == List + 24 || pair.first == Node + 20 ||
                  pair.first == A + 20 || pair.first == B + 20 ||
                  pair.first == Payload + 8 + 19 * 4
              ? 4
              : 8);
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.put(B, A);
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.put(B + 8, 7);
    m.put(B + 16, 7, 4);
    unknown(m);
    ++cases;
  }
  for (auto at : {uint64_t(Payload), uint64_t(Node), uint64_t(A),
                  uint64_t(R + 16), uint64_t(Base + 0x34bc3c8)}) {
    Memory m;
    m.before = [&](uint64_t p, size_t v) {
      if (p == at && v == 2)
        m.put(Payload + 8 + 19 * 4, 7, 4);
    };
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.m.erase(Payload + 435);
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.query.target_ordinal = 20;
    auto x = m.run();
    assert(!x.known && m.reads == 0);
    ++cases;
  }
  for (auto at : {uint64_t(Base + 0x34bc3a8), uint64_t(Buckets),
                  uint64_t(StageBuckets)}) {
    Memory m;
    m.put(at, 0);
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.put(A + 16, 8, 4);
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.query.progress_crcs[1] = m.query.progress_crcs[0];
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.source.registry_entries += 8;
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.source.save_epoch.revision++;
    unknown(m);
    ++cases;
  }
  {
    Memory m;
    m.put(Base + 0x34bc3d0, UINT64_MAX);
    unknown(m);
    ++cases;
  }
  assert(!LoadFieldStoryQuery({}).valid);
  ++cases;
  assert(!LoadFieldStoryQuery([](std::string_view, uint64_t, void *, size_t) {
            return size_t(1048577);
          }).valid);
  ++cases;
  const char *root = std::getenv("ECHOES_ROMFS");
  if (root) {
    std::map<std::string, std::vector<uint8_t>> files;
    for (auto path : {"region_common/datasheets/ui/UiMapExtend.gsheet",
                      "region_common/datasheets/Quest.gsheet"}) {
      std::ifstream in(std::filesystem::path(root) / path, std::ios::binary);
      assert(in);
      files[std::string("romfs:/") + path] = {
          (std::istreambuf_iterator<char>(in)), {}};
    }
    auto reader = [&](std::string_view path, uint64_t off, void *out,
                      size_t n) -> size_t {
      auto i = files.find(std::string(path));
      if (i == files.end())
        return 0;
      auto &b = i->second;
      if (!out)
        return b.size();
      if (off > b.size() || n > b.size() - off)
        return 0;
      std::memcpy(out, b.data() + off, n);
      return n;
    };
    auto q = LoadFieldStoryQuery(reader);
    assert(q.valid && q.quest_count == 107 && q.stage_count == 20 &&
           q.quest_index == 19 && q.target_ordinal == 6);
    ++cases;
    auto &ui =
        files.at("romfs:/region_common/datasheets/ui/UiMapExtend.gsheet");
    auto copy = ui;
    ui[4] = 3;
    assert(!LoadFieldStoryQuery(reader).valid);
    ui = copy;
    ++cases;
    // Shared supported expression must not silently become an OR/another
    // operator.
    auto needle = std::string("<=");
    auto at = std::search(ui.begin(), ui.end(), needle.begin(), needle.end());
    assert(at != ui.end());
    *at = '!';
    assert(!LoadFieldStoryQuery(reader).valid);
    ui = copy;
    ++cases;
    auto get64 = [](const std::vector<uint8_t> &b, size_t p) {
      uint64_t v = 0;
      for (unsigned i = 0; i < 8; ++i)
        v |= uint64_t(b[p + i]) << (8 * i);
      return v;
    };
    const auto rows = get64(ui, 32);
    const auto info = get64(ui, rows + 16);
    // A malformed nested expression count must fail before following its data.
    for (unsigned i = 0; i < 8; ++i)
      ui[info + 32 + i] = 255;
    assert(!LoadFieldStoryQuery(reader).valid);
    ui = copy;
    ++cases;
    // A duplicated native Field row is ambiguous, even if expressions match.
    std::copy_n(ui.begin() + rows, 32, ui.begin() + rows + 32);
    assert(!LoadFieldStoryQuery(reader).valid);
    ui = copy;
    ++cases;
    for (unsigned i = 0; i < 8; ++i)
      ui[rows + i] = 255;
    assert(!LoadFieldStoryQuery(reader).valid);
    ui = copy;
    ++cases;
    auto &quest = files.at("romfs:/region_common/datasheets/Quest.gsheet");
    quest[40] = 106;
    assert(!LoadFieldStoryQuery(reader).valid);
    ++cases;
    std::cout << "native original story metadata validated\n";
  }
  std::cout << cases << " story reader/catalog cases passed\n";
}
