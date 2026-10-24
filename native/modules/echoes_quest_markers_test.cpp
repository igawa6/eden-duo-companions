// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_catalog.h"
#include "echoes_quest_markers.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
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
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
constexpr uint64_t Base = 0x80000000, Owner = 0x90000000, Save = 0x90010000,
                   List = 0x90040000, Traverse = 0x90041000,
                   Registry = 0x90042000, Header = 0x90043000,
                   Entries = 0x90044000, QuestHashes = 0x90100000,
                   StageVector = 0x90200000, StageHashes = 0x91000000;
struct Memory {
  std::unordered_map<uint64_t, uint8_t> b;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  const std::vector<uint8_t> &text;
  MapEpochSource source{
      {Owner, Save, List, 7}, Traverse, Registry, Header, Entries};
  void P(uint64_t p, uint64_t v, size_t n = 8) {
    for (size_t i = 0; i < n; ++i)
      b[p + i] = uint8_t(v >> (8 * i));
  }
  void Zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      b[p + i] = 0;
  }
  void Hash(uint64_t h, uint64_t storage, std::span<const uint32_t> keys) {
    Zero(h, 40);
    P(h, storage);
    P(h + 8, 1);
    P(h + 16, storage + 32);
    P(h + 24, keys.size());
    P(h + 32, 0x3f800000, 4);
    P(storage, storage + 8);
    P(storage + 8, storage + 32);
    for (size_t i = 0; i < keys.size(); ++i) {
      const auto node = storage + 32 + i * 32;
      Zero(node, 24);
      P(node, i + 1 < keys.size() ? node + 32 : 0);
      P(node + 8, keys[i]);
      P(node + 16, keys[i], 4);
      P(node + 20, i, 4);
    }
  }
  Memory(const std::vector<uint8_t> &t,
         std::span<const NativeQuestMarkerQuest> q)
      : text(t) {
    P(Base + 0x34bc848, Owner);
    P(Owner + 0x10, Save);
    P(Save + 0x16b80, Traverse);
    P(Save + 0x16cf8, List);
    Zero(List, 32);
    P(List + 16, 127, 4);
    P(List + 24, 7, 4);
    P(Base + 0x34b80b8, Registry);
    P(Registry + 0x78, Header);
    P(Header + 0x20, Entries);
    P(Save + 0x16a70, Save + 0xbcf0);
    P(Save + 0x16d30, Save + 0xffa0);
    Zero(Save + 0x16d38, 32);
    P(Save + 0x16d38, Base + 0x33ff808);
    P(Base + 0x33ff828, Base + 0x232a9b0);
    P(Save + 0x16d40, 123, 4);
    P(Save + 0x16d48, Save + 0xffb0);
    P(Save + 0x16d50, 256, 4);
    Zero(Save + 0xffb0, 8 + q.size() * 4);
    P(Save + 0xffb0, q.size(), 4);
    P(Save + 0xffb4, 123, 4);
    P(Base + 0x34bc3f8, 1, 1);
    P(Base + 0x34bc390, 0x90300000);
    P(Base + 0x34bc3c8, StageVector);
    P(Base + 0x34bc3d0, StageVector + q.size() * 40);
    P(Base + 0x34bc3d8, StageVector + q.size() * 40);
    std::vector<uint32_t> keys;
    for (size_t i = 0; i < q.size(); ++i) {
      keys.push_back(ActorKey(q[i].key));
      Hash(StageVector + i * 40, StageHashes + i * 0x4000, q[i].progress_crcs);
      Stage(i, q[i], 0);
    }
    Hash(Base + 0x34bc3a0, QuestHashes, keys);
    Zero(Save + 0x103b8, 8);
    Zero(Save + 0x1043c, 4);
  }
  void Stage(size_t index, const NativeQuestMarkerQuest &q, size_t ordinal) {
    P(Save + 0xffb8 + index * 4, q.progress_crcs.at(ordinal), 4);
  }
  void Track(const NativeQuestMarkerQuest &q) {
    P(Save + 0x103b8 + q.kind, 1, 1);
    P(Save + (q.kind ? 0x1043c : 0x103bc), ActorKey(q.key), 4);
  }
  bool Read(uint64_t p, void *out, size_t n) {
    const auto v = ++visits[p];
    if (before)
      before(p, v);
    if (n > UINT64_MAX - p)
      return false;
    for (size_t i = 0; i < n; ++i) {
      auto it = b.find(p + i);
      if (it != b.end())
        static_cast<uint8_t *>(out)[i] = it->second;
      else if (p + i >= Base && p + i - Base < text.size())
        static_cast<uint8_t *>(out)[i] = text[size_t(p + i - Base)];
      else
        return false;
    }
    return true;
  }
  auto Snapshot(std::span<const NativeQuestMarkerQuest> q,
                std::span<const NativeCheckpointGroup> g, bool build = true) {
    return ReadCheckpointVisibility(
        [&](auto p, auto o, auto n) { return Read(p, o, n); }, Base, build,
        source, q, g);
  }
};
} // namespace
int main() {
  std::vector<NativeQuestMarkerQuest> q(1);
  Check(!LoadMarkerQuestCatalog({}, q) && q.empty(),
        "missing original quest source");
  const auto romfs = std::getenv("ECHOES_ROMFS"),
             segments = std::getenv("ECHOES_SEGMENTS");
  if (!romfs || !segments) {
    std::cout << checks << " checks; fixtures unavailable\n";
    return 77;
  }
  const auto text = File(std::filesystem::path(segments) / "text.bin");
  const auto read = [&](std::string_view path, uint64_t off, void *out,
                        size_t n) -> size_t {
    const auto b = File(std::filesystem::path(romfs) / std::string(path));
    if (!out && !n)
      return b.size();
    if (off > b.size() || n > b.size() - off)
      return 0;
    std::copy_n(b.data() + off, n, static_cast<uint8_t *>(out));
    return n;
  };
  Check(LoadMarkerQuestCatalog(read, q) && q.size() == 107,
        "all original quests include three authored kinds");
  std::vector<NativeCheckpointGroup> groups;
  Check(LoadNativeCheckpoints(read, "Field", groups) && groups.size() == 73,
        "original Field marker groups");
  const auto selected = std::find_if(q.begin(), q.end(), [](const auto &v) {
    return v.key == "Animal_Coast010";
  });
  Check(selected != q.end() && selected->kind == 1 &&
            selected->progress_crcs.size() > 2,
        "original side quest active range");
  const auto qi = size_t(selected - q.begin());
  {
    Memory m(text, q);
    const auto s = m.Snapshot(q, groups);
    Check(s.compatible && s.known && s.visible.empty(),
          "no tracked quest emits no invented circles");
  }
  for (size_t stage :
       {size_t(0), size_t(1), selected->progress_crcs.size() - 1}) {
    Memory m(text, q);
    m.Track(*selected);
    m.Stage(qi, *selected, stage);
    const auto s = m.Snapshot(q, groups);
    Check(s.compatible && s.known && s.visible.size() == (stage == 1 ? 1 : 0) &&
              s.source == m.source,
          "native active quest gate handles unstarted active completed");
  }
  {
    Memory m(text, q);
    m.Track(*selected);
    m.Stage(qi, *selected, 1);
    m.P(Save + 0x103b9, 0, 1);
    const auto s = m.Snapshot(q, groups);
    Check(s.known && s.visible.empty(),
          "native tracked display toggle hides markers");
  }
  const auto equality_quest =
      std::find_if(q.begin(), q.end(), [](const auto &v) {
        return v.key == "Animal_HyrulePlains010";
      });
  Check(equality_quest != q.end() && equality_quest->kind == 1,
        "original staged marker quest");
  const auto eqi = size_t(equality_quest - q.begin());
  const auto group =
      std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
        return g.quest_crc == ActorKey(equality_quest->key);
      });
  Check(group != groups.end() && !group->markers.empty(),
        "original staged marker group");
  const auto expression = group->markers[0].conditions[0].parameter;
  const auto target = expression.substr(expression.find('=') + 1);
  const auto ordinal_it =
      std::find(equality_quest->progress_crcs.begin(),
                equality_quest->progress_crcs.end(), ActorKey(target));
  Check(ordinal_it != equality_quest->progress_crcs.end(),
        "original target stage is catalog member");
  const auto target_index =
      size_t(ordinal_it - equality_quest->progress_crcs.begin());
  for (const auto &op : {"=", "==", "!=", "<", ">", "<=", ">="})
    for (size_t stage :
         {size_t(0), target_index, equality_quest->progress_crcs.size() - 1}) {
      auto test = *group;
      test.markers.resize(1);
      test.markers[0].conditions = {{3, equality_quest->key + op + target}};
      Memory m(text, q);
      m.Track(*equality_quest);
      m.Stage(eqi, *equality_quest, stage);
      const auto s = m.Snapshot(q, std::span(&test, 1));
      const std::string_view o = op;
      const bool expected = o == "=" || o == "==" ? stage == target_index
                            : o == "!="           ? stage != target_index
                            : o == "<"            ? stage < target_index
                            : o == ">"            ? stage > target_index
                            : o == "<="           ? stage <= target_index
                                                  : stage >= target_index;
      Check(s.known && s.visible.size() == size_t(expected),
            "exact original unsigned comparison including boundary");
    }
  {
    auto test = *group;
    test.markers.resize(1);
    test.markers[0].conditions = {
        {3, equality_quest->key + ":" + target + "~" + target}};
    for (size_t stage :
         {size_t(0), target_index, equality_quest->progress_crcs.size() - 1}) {
      Memory m(text, q);
      m.Track(*equality_quest);
      m.Stage(eqi, *equality_quest, stage);
      const auto s = m.Snapshot(q, std::span(&test, 1));
      Check(s.known && s.visible.size() == size_t(stage == target_index),
            "inclusive original range and both boundaries");
    }
  }
  for (int fault = 0; fault < 14; ++fault) {
    Memory m(text, q);
    m.Track(*selected);
    m.Stage(qi, *selected, 1);
    if (fault == 0)
      m.P(Save + 0x16a70, Save + 0xbcf8);
    if (fault == 1)
      m.P(Save + 0x16d30, Save + 0xffa8);
    if (fault == 2)
      m.P(Save + 0x16d48, Save + 0xffb8);
    if (fault == 3)
      m.P(Save + 0x16d38, Base);
    if (fault == 4)
      m.P(Save + 0x16d50, 257, 4);
    if (fault == 5)
      m.P(Save + 0xffb0, 106, 4);
    if (fault == 6)
      m.P(Save + 0xffb4, 124, 4);
    if (fault == 7)
      m.P(Save + 0x1043c, 0xfefefefe, 4);
    if (fault == 8)
      m.P(Save + 0xffb8 + qi * 4, 0, 4);
    if (fault == 9)
      m.P(Base + 0x34bc3f8, 0, 1);
    if (fault == 10)
      m.P(List + 24, 8, 4);
    if (fault == 11)
      m.before = [&](uint64_t p, size_t n) {
        if (p == Save + 0x103b8 && n == 2)
          m.P(Save + 0x103b9, 0, 1);
      };
    if (fault == 12)
      m.before = [&](uint64_t p, size_t n) {
        if (p == Save + 0x16d30 && n == 3)
          m.P(Save + 0x16d30, Save + 0xffa8);
      };
    if (fault == 13)
      m.P(Base + 0x232d1c8, 0, 4);
    const auto s = m.Snapshot(q, groups);
    Check(!s.known && s.visible.empty(),
          "broken ownership typed source stage or transitions fail closed");
  }
  {
    auto test = *group;
    test.markers.resize(1);
    Memory m(text, q);
    m.Track(*equality_quest);
    m.Stage(eqi, *equality_quest, target_index);
    test.markers[0].conditions = {{99, "unknown"}};
    auto s = m.Snapshot(q, std::span(&test, 1));
    Check(s.known && s.visible.empty() && s.unsupported_markers == 1,
          "unsupported marker gate omitted explicitly");
    test.markers[0].conditions.clear();
    m.visits.clear();
    s = m.Snapshot(q, std::span(&test, 1));
    Check(s.known && s.visible.empty(), "native empty AND is false");
  }
  {
    Memory m(text, q);
    const auto s = m.Snapshot(q, groups, false);
    Check(!s.compatible && !s.known && m.visits.empty(),
          "unsupported build reads nothing");
  }
  size_t covered_groups = 0, unsupported = 0;
  for (const auto &g : groups) {
    const auto it = std::find_if(q.begin(), q.end(), [&](const auto &v) {
      return ActorKey(v.key) == g.quest_crc;
    });
    if (it == q.end() || it->kind > 1)
      continue;
    Memory m(text, q);
    m.Track(*it);
    m.Stage(size_t(it - q.begin()), *it,
            std::min(size_t(1), it->progress_crcs.size() - 1));
    const auto state = m.Snapshot(q, std::span(&g, 1));
    Check(state.known, "original authored checkpoint predicates authenticate");
    ++covered_groups;
    unsupported += state.unsupported_markers;
  }
  Check(covered_groups == 66 && unsupported == 2,
        "all trackable original groups authenticate; two absent stage references stay hidden");
  std::cout << "Original authored groups qualified: " << covered_groups
            << "; unsupported predicates: " << unsupported << '\n';
  std::cout << checks << " checks passed\n";
}
