// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_story.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
namespace echoes {
namespace {
uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t *p) {
  return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32;
}
uint32_t CRC(std::string_view s) {
  uint32_t v = ~uint32_t(0);
  for (unsigned char c : s) {
    v ^= c;
    for (int i = 0; i < 8; ++i)
      v = (v >> 1) ^ (0xedb88320u & uint32_t(-int32_t(v & 1)));
  }
  return ~v;
}
uint64_t Add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::runtime_error("range");
  return a + b;
}
struct Sheet {
  std::vector<uint8_t> b;
  uint64_t pool{}, rows{};
  uint32_t count{}, stride{}, fields{}, top{};
  void Fit(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("sheet range");
  }
  uint32_t D(uint64_t p) const {
    Fit(p, 4);
    return U32(b.data() + p);
  }
  uint64_t Q(uint64_t p) const {
    Fit(p, 8);
    return U64(b.data() + p);
  }
  std::string C(uint64_t p) const {
    if (p < pool)
      throw std::runtime_error("pool");
    std::string s;
    for (size_t n = 0; n < 256; ++n) {
      Fit(p + n, 1);
      auto c = b[p + n];
      if (!c)
        return s;
      s += char(c);
    }
    throw std::runtime_error("text");
  }
  std::string Text(uint64_t at) const {
    auto p = Q(at), n = Q(at + 8);
    if (!n || n > 255 || p < pool)
      throw std::runtime_error("string");
    Fit(p, n + 1);
    if (b[p + n])
      throw std::runtime_error("termination");
    std::string s(reinterpret_cast<const char *>(b.data() + p), size_t(n));
    if (s.find('\0') != s.npos)
      throw std::runtime_error("nul");
    return s;
  }
  uint64_t Field(uint64_t first, size_t n, std::string_view name, uint8_t type,
                 uint16_t flags, uint16_t off, uint16_t size,
                 uint16_t step) const {
    uint64_t found = 0;
    for (size_t i = 0; i < n; ++i) {
      auto p = Add(first, i * 48);
      Fit(p, 48);
      if (C(Q(p)) != name)
        continue;
      if (found || b[p + 16] != type || (D(p + 18) & 65535) != flags ||
          (D(p + 20) & 65535) != off || (D(p + 22) & 65535) != size ||
          (D(p + 24) & 65535) != step)
        throw std::runtime_error("schema");
      found = p;
    }
    if (!found)
      throw std::runtime_error("field");
    return found;
  }
  uint64_t Children(uint64_t f, size_t expected) const {
    if ((D(f + 26) & 65535) != expected)
      throw std::runtime_error("children");
    auto p = Q(f + 32);
    if (p < 48 || (p - 48) % 48 || expected > fields ||
        (p - 48) / 48 > fields - expected)
      throw std::runtime_error("descriptors");
    return p;
  }
  std::pair<uint64_t, uint64_t> Array(uint64_t at, uint64_t step,
                                      uint64_t max) const {
    auto p = Q(at), n = Q(at + 8);
    if (n > max)
      throw std::runtime_error("array");
    Fit(p, n * step);
    return {p, n};
  }
  Sheet(const StoryReadAssetFn &r, std::string_view path) {
    auto n = r(path, 0, nullptr, 0);
    if (n < 48 || n > 1024 * 1024)
      throw std::runtime_error("asset budget");
    b.resize(n);
    if (r(path, 0, b.data(), n) != n || r(path, 0, nullptr, 0) != n)
      throw std::runtime_error("asset changed");
    if (std::memcmp(b.data(), "gsht\2\0\0\0", 8))
      throw std::runtime_error("format");
    pool = Q(16);
    top = D(24);
    fields = D(28);
    rows = Q(32);
    count = D(40);
    stride = D(44);
    if (fields > 128 || top > fields || count > 2048 || stride > 512 ||
        rows < 48 + fields * 48 || pool < rows || !stride)
      throw std::runtime_error("header");
    Fit(48, fields * 48);
    Fit(rows, uint64_t(count) * stride);
    Fit(pool, 1);
  }
};
bool Identifier(std::string_view s) {
  return !s.empty() && s.size() <= 128 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}
struct ReadTrace {
  const ReadFn &r;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
  std::vector<uint8_t> Bytes(uint64_t at, size_t n) {
    if (!at || n > UINT64_MAX - at)
      throw std::runtime_error("address");
    std::vector<uint8_t> b(n);
    if (!r(at, b.data(), n))
      throw std::runtime_error("read");
    records.emplace_back(at, b);
    return b;
  }
  uint64_t Q(uint64_t at) {
    if (at % 8)
      throw std::runtime_error("alignment");
    auto b = Bytes(at, 8);
    return U64(b.data());
  }
  uint64_t Pointer(uint64_t at) {
    auto v = Q(at);
    if (!v || v % 8)
      throw std::runtime_error("pointer");
    return v;
  }
};
uint32_t Lookup(ReadTrace &t, uint64_t at, uint32_t key,
                uint32_t expected_count, uint32_t max_value) {
  auto h = t.Bytes(at, 40);
  auto map = U64(h.data()), buckets = U64(h.data() + 8),
       size = U64(h.data() + 24);
  float load = std::bit_cast<float>(U32(h.data() + 32));
  if (!map || map % 8 || !buckets || buckets > 4096 || size != expected_count ||
      !size || !std::isfinite(load) || load <= 0)
    throw std::runtime_error("hash header");
  auto bucket = key % buckets;
  auto predecessor = t.Pointer(Add(map, bucket * 8));
  auto node = t.Pointer(predecessor);
  std::set<uint64_t> seen;
  bool found = false;
  uint32_t value = 0;
  while (node) {
    if (node % 8 || seen.size() >= size || !seen.insert(node).second)
      throw std::runtime_error("hash cycle");
    auto n = t.Bytes(node, 24);
    auto hash = U64(n.data() + 8);
    if (hash % buckets != bucket)
      break;
    if (hash == key && U32(n.data() + 16) == key) {
      if (found)
        throw std::runtime_error("duplicate");
      value = U32(n.data() + 20);
      if (value >= max_value)
        throw std::runtime_error("index");
      found = true;
    }
    node = U64(n.data());
  }
  if (!found)
    throw std::runtime_error("absent");
  return value;
}
struct Frame {
  FieldStorySnapshot result;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> trace;
  bool operator==(const Frame &) const = default;
};
struct SandstormFrame {
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> trace;
  uint32_t state{};
  bool operator==(const SandstormFrame &) const = default;
};
SandstormFrame SampleSandstorm(const ReadFn &r, uint64_t base,
                              const MapEpochSource &source) {
  ReadTrace t{r, {}};
  auto owner = t.Pointer(Add(base, 0x34bc848)),
       save = t.Pointer(Add(owner, 0x10));
  if (owner != source.save_epoch.access_object ||
      save != source.save_epoch.save_data ||
      t.Pointer(Add(save, 0x16b80)) != source.traversal ||
      t.Pointer(Add(save, 0x16cf8)) != source.save_epoch.list)
    throw std::runtime_error("save epoch");
  auto learned = t.Bytes(source.save_epoch.list, 32);
  if (U32(learned.data() + 24) != source.save_epoch.revision ||
      U32(learned.data() + 16) > 512)
    throw std::runtime_error("collection epoch");
  if (t.Pointer(Add(base, 0x34b80b8)) != source.registry_owner ||
      t.Pointer(Add(source.registry_owner, 0x78)) != source.registry_header ||
      t.Pointer(Add(source.registry_header, 0x20)) != source.registry_entries)
    throw std::runtime_error("map epoch");
  if (!(t.Bytes(Add(base, 0x34bd8e8), 1)[0] & 1) ||
      !(t.Bytes(Add(base, 0x34a9140), 1)[0] & 1) ||
      t.Q(Add(base, 0x34a9138)) != Add(base, 0x326a030))
    throw std::runtime_error("scene initialization");
  auto manager = t.Pointer(Add(base, 0x34bd718));
  auto scene = t.Pointer(Add(manager, 0x80));
  // Exact Main scene identity substitutes for the native type-check virtual call.
  if (t.Q(scene) != Add(base, 0x3268098) ||
      t.Q(Add(base, 0x32680c0)) != Add(base, 0x165b4a8))
    throw std::runtime_error("main scene class");
  auto typed = t.Pointer(Add(scene, 0xa0));
  (void)t.Pointer(Add(scene, 0xa8));
  auto weather = t.Pointer(Add(typed, 0x140));
  if (t.Q(weather) != Add(base, 0x329fcd0) ||
      t.Q(Add(base, 0x329fcd0)) != Add(base, 0x17a26c0) ||
      t.Q(Add(base, 0x329fcd8)) != Add(base, 0x17a2720))
    throw std::runtime_error("weather class");
  auto state = U32(t.Bytes(Add(weather, 0x20), 4).data());
  // Original weather selection emits 0..6. Unknown/sentinel states retain fog.
  if (state > 6)
    throw std::runtime_error("weather enum");
  return {std::move(t.records), state};
}
Frame Sample(const ReadFn &r, uint64_t base, const MapEpochSource &source,
             const StoryQuery &q) {
  ReadTrace t{r, {}};
  Frame f;
  auto &v = f.result;
  auto owner = t.Pointer(Add(base, 0x34bc848)),
       save = t.Pointer(Add(owner, 0x10));
  if (owner != source.save_epoch.access_object ||
      save != source.save_epoch.save_data ||
      t.Pointer(Add(save, 0x16b80)) != source.traversal ||
      t.Pointer(Add(save, 0x16cf8)) != source.save_epoch.list)
    throw std::runtime_error("save epoch");
  auto learned = t.Bytes(source.save_epoch.list, 32);
  if (U32(learned.data() + 24) != source.save_epoch.revision ||
      U32(learned.data() + 16) > 512)
    throw std::runtime_error("collection epoch");
  if (t.Pointer(Add(base, 0x34b80b8)) != source.registry_owner ||
      t.Pointer(Add(source.registry_owner, 0x78)) != source.registry_header ||
      t.Pointer(Add(source.registry_header, 0x20)) != source.registry_entries)
    throw std::runtime_error("map epoch");
  auto obj = t.Bytes(Add(save, 0x16d38), 32);
  if (U64(obj.data()) != Add(base, 0x33ff808) ||
      t.Q(Add(base, 0x33ff828)) != Add(base, 0x232a9b0))
    throw std::runtime_error("quest class");
  auto payload = U64(obj.data() + 16);
  auto capacity = U32(obj.data() + 24);
  if (!payload || payload % 8 || capacity < q.quest_count || capacity > 256)
    throw std::runtime_error("quest capacity");
  auto bytes = t.Bytes(payload, 8 + q.quest_count * 4);
  if (U32(bytes.data()) != q.quest_count ||
      U32(bytes.data() + 4) != U32(obj.data() + 8))
    throw std::runtime_error("quest header");
  auto initialized = t.Bytes(Add(base, 0x34bc3f8), 1);
  if (!(initialized[0] & 1))
    throw std::runtime_error("uninitialized");
  auto resource = t.Pointer(Add(base, 0x34bc390));
  auto quest = Lookup(t, Add(base, 0x34bc3a0), q.quest_crc, q.quest_count,
                      q.quest_count);
  if (quest != q.quest_index)
    throw std::runtime_error("metadata quest");
  auto vec = t.Bytes(Add(base, 0x34bc3c8), 24);
  auto begin = U64(vec.data()), end = U64(vec.data() + 8),
       cap = U64(vec.data() + 16);
  if (!begin || begin % 8 || end % 8 || cap % 8 || end < begin || cap < end ||
      (end - begin) % 40 || (cap - begin) % 40 ||
      (end - begin) / 40 != q.quest_count || (cap - begin) / 40 > 128)
    throw std::runtime_error("stage vector");
  auto stage = Add(begin, uint64_t(quest) * 40);
  auto target = Lookup(t, stage, q.target_crc, q.stage_count, q.stage_count);
  if (target != q.target_ordinal)
    throw std::runtime_error("metadata stage");
  uint32_t current = U32(bytes.data() + 8 + quest * 4), ordinal{};
  // Unrecognized/sentinel stages fail closed in this first slice. They can keep
  // coverage without reproducing the engine's default/special ordinal behavior.
  auto at = std::find(q.progress_crcs.begin(), q.progress_crcs.end(), current);
  if (at == q.progress_crcs.end())
    throw std::runtime_error("unknown stage");
  ordinal = Lookup(t, stage, current, q.stage_count, q.stage_count);
  if (ordinal != size_t(at - q.progress_crcs.begin()))
    throw std::runtime_error("metadata current");
  v.known = true;
  v.native_field_extension_visible = ordinal <= target;
  v.map_source = source;
  v.save_owner = owner;
  v.save_data = save;
  v.payload = payload;
  v.metadata_resource = resource;
  v.stage_header = stage;
  v.current_crc = current;
  v.current_ordinal = ordinal;
  v.target_ordinal = target;
  f.trace = std::move(t.records);
  return f;
}
} // namespace
StoryQuery LoadFieldStoryQuery(const StoryReadAssetFn &r) {
  StoryQuery q;
  if (!r)
    return q;
  try {
    Sheet ui(r, "romfs:/region_common/datasheets/ui/UiMapExtend.gsheet");
    if (ui.top != 2 || ui.stride != 32 || ui.count != 2)
      return {};
    ui.Field(48, 2, "mKey", 4, 4, 0, 16, 16);
    auto ex = ui.Field(48, 2, "mExtendInfo", 0, 2, 16, 16, 40);
    auto e = ui.Children(ex, 2);
    ui.Field(e, 2, "mPaneName", 4, 0, 0, 16, 16);
    auto vis = ui.Field(e, 2, "mVisibleCondition", 0, 0, 16, 24, 24);
    auto c = ui.Children(vis, 2);
    ui.Field(c, 2, "mStrategy", 2, 48, 0, 4, 4);
    auto expr = ui.Field(c, 2, "mExpressions", 0, 2, 8, 16, 24);
    auto t = ui.Children(expr, 2);
    ui.Field(t, 2, "mCategory", 2, 16, 0, 4, 4);
    ui.Field(t, 2, "mParameter", 4, 0, 8, 16, 16);
    std::string expression;
    bool field = false;
    for (uint32_t i = 0; i < ui.count; ++i) {
      auto row = ui.rows + uint64_t(i) * 32;
      if (ui.Text(row) != "Field")
        continue;
      if (field)
        return {};
      field = true;
      auto [p, n] = ui.Array(row + 16, 40, 3);
      if (n != 3)
        return {};
      for (size_t j = 0; j < 3; ++j) {
        auto entry = p + j * 40;
        q.pane_names[j] = ui.Text(entry);
        if (!Identifier(q.pane_names[j]) ||
            std::find(q.pane_names.begin(), q.pane_names.begin() + j,
                      q.pane_names[j]) != q.pane_names.begin() + j ||
            ui.D(entry + 16) != 0)
          return {};
        auto [xp, xn] = ui.Array(entry + 24, 24, 1);
        if (xn != 1 || ui.D(xp) != 16)
          return {};
        auto value = ui.Text(xp + 8);
        if (j && value != expression)
          return {};
        expression = value;
      }
    }
    if (!field)
      return {};
    auto op = expression.find("<=");
    if (op == expression.npos ||
        expression.find("<=", op + 2) != expression.npos)
      return {};
    auto name = expression.substr(0, op), label = expression.substr(op + 2);
    if (!Identifier(name) || !Identifier(label))
      return {};
    Sheet quests(r, "romfs:/region_common/datasheets/Quest.gsheet");
    if (quests.count != 107 || quests.stride != 104 || quests.top != 7)
      return {};
    quests.Field(48, 7, "mKey", 4, 4, 0, 16, 16);
    auto pr = quests.Field(48, 7, "mProgress", 0, 2, 56, 16, 24);
    auto child = quests.Children(pr, 3);
    quests.Field(child, 3, "mLabel", 2, 0, 0, 4, 4);
    std::set<uint32_t> keys;
    bool found = false;
    q.quest_crc = CRC(name);
    q.target_crc = CRC(label);
    for (uint32_t i = 0; i < quests.count; ++i) {
      auto row = quests.rows + uint64_t(i) * 104;
      auto key = quests.Text(row);
      if (!Identifier(key) || !keys.insert(CRC(key)).second)
        return {};
      if (key != name)
        continue;
      if (found)
        return {};
      found = true;
      q.quest_index = i;
      auto [p, n] = quests.Array(row + 56, 24, 20);
      if (n != 20)
        return {};
      std::set<uint32_t> stages;
      for (size_t j = 0; j < n; ++j) {
        auto crc = quests.D(p + j * 24);
        if (!crc || crc == UINT32_MAX || !stages.insert(crc).second)
          return {};
        q.progress_crcs.push_back(crc);
        if (crc == q.target_crc)
          q.target_ordinal = static_cast<uint32_t>(j);
      }
      if (!stages.contains(q.target_crc))
        return {};
      q.stage_count = static_cast<uint32_t>(n);
    }
    if (!found)
      return {};
    q.quest_count = quests.count;
    q.valid = true;
    return q;
  } catch (...) {
    return {};
  }
}
FieldStorySnapshot ReadFieldStory(const ReadFn &r, uint64_t base, bool build,
                                  const MapEpochSource &source,
                                  const StoryQuery &q) {
  if (!r || !build || !base || base % 4096 || !q.valid ||
      q.quest_count != 107 || q.stage_count != 20 ||
      q.progress_crcs.size() != 20 || q.quest_index >= 107 ||
      q.target_ordinal >= 20 ||
      q.progress_crcs[q.target_ordinal] != q.target_crc || !q.quest_crc)
    return {};
  FieldStorySnapshot result;
  try {
    std::set<uint32_t> unique;
    for (auto crc : q.progress_crcs)
      if (!crc || crc == UINT32_MAX || !unique.insert(crc).second)
        return {};
    auto guard = [&](const IdentityGuard &g) {
      std::array<uint8_t, 256> b{};
      auto at = Add(base, g.offset);
      return r(at, b.data(), b.size()) && MatchSupportedGuard(b, g.bytes, g.offset);
    };
    for (auto &g : IdentityGuards)
      if (!guard(g))
        return {};
    try {
      for (auto &g : SandstormIdentityGuards)
        if (!guard(g))
          throw std::runtime_error("weather original bytes");
      auto a = SampleSandstorm(r, base, source),
           b = SampleSandstorm(r, base, source),
           c = SampleSandstorm(r, base, source);
      if (a != b || a != c)
        throw std::runtime_error("weather changed");
      result.sandstorm_known = true;
      result.sandstorm_visible = a.state == 5;
      result.map_source = source;
    } catch (...) {
      // Quest visibility remains independently available when weather is unknown.
    }
    for (auto &g : FieldStoryIdentityGuards)
      if (!guard(g))
        return result;
    auto a = Sample(r, base, source, q), b = Sample(r, base, source, q),
         c = Sample(r, base, source, q);
    if (a != b || a != c)
      return result;
    a.result.sandstorm_known = result.sandstorm_known;
    a.result.sandstorm_visible = result.sandstorm_visible;
    return a.result;
  } catch (...) {
    return result;
  }
}
} // namespace echoes
