// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle_status.h"
#include "ot0_battle_actors.h"
#include <cstring>
namespace ot0 {
namespace {
using namespace battle_detail;
template <class T> T At(const uint8_t *p) {
  T v{};
  std::memcpy(&v, p, sizeof v);
  return v;
}
struct StatusSample {
  Array raw{}, visible{};
  std::vector<std::array<uint8_t, 0xe8>> rows;
  std::vector<std::array<uint8_t, 24>> icons;
  bool operator==(const StatusSample &) const = default;
};
bool SampleStatuses(const Reader &r, uint64_t actor, StatusSample &out) {
  if (!r.get(actor + 0x800, out.raw) || !r.get(actor + 0x858, out.visible))
    return false;
  const auto bounded = [](const Array &a) {
    return a.count >= 0 && a.count <= 128 && a.capacity >= a.count &&
           a.capacity <= 256 && (!a.count || Pointer(a.data));
  };
  if (!bounded(out.raw) || !bounded(out.visible))
    return false;
  out.rows.resize(size_t(out.raw.count));
  out.icons.resize(size_t(out.visible.count));
  for (int i = 0; i < out.raw.count; ++i)
    if (!r.get(out.raw.data + uint64_t(i) * 0xe8, out.rows[size_t(i)]))
      return false;
  for (int i = 0; i < out.visible.count; ++i)
    if (!r.get(out.visible.data + uint64_t(i) * 24, out.icons[size_t(i)]))
      return false;
  return true;
}
bool NativeName(const Reader &r, uint32_t index, uint32_t number,
                std::string &out) {
  if (index >> 16 > 8191 || number > 1000000000)
    return false;
  uint64_t block{}, again{};
  uint16_t head{}, head2{};
  const uint64_t slot = r.main + 0x6c91440 + 0x38 + 8 * (index >> 16);
  if (!r.ptr(slot, block))
    return false;
  uint64_t entry = block + 2 * (index & 65535);
  if (!r.get(entry, head))
    return false;
  unsigned length = head >> 6;
  if (!length || length > 1023)
    return false;
  std::vector<uint8_t> bytes(length * (head & 1 ? 2 : 1)),
      checked(bytes.size());
  if (entry >= Limit || bytes.size() + 2 > Limit - entry ||
      !r.read(entry + 2, bytes.data(), bytes.size()))
    return false;
  for (unsigned i = 0; i < length; ++i) {
    uint32_t c = head & 1 ? At<uint16_t>(bytes.data() + i * 2) : bytes[i];
    if (c < 32 || c > 126)
      return false;
    out += char(c);
  }
  if (number)
    out += "_" + std::to_string(number - 1);
  return r.ptr(slot, again) && again == block && r.get(entry, head2) &&
         head == head2 && r.read(entry + 2, checked.data(), checked.size()) &&
         checked == bytes;
}
std::string TexturePath(const Reader &r, uint64_t object,
                        std::string &asset_class) {
  if (!Pointer(object))
    return {};
  std::array<uint8_t, 0x28> header{}, cls{}, package{}, checked{};
  if (!r.get(object, header) || (At<uint32_t>(header.data() + 8) & 0x40000030))
    return {};
  uint64_t klass = At<uint64_t>(header.data() + 0x10),
           outer = At<uint64_t>(header.data() + 0x20);
  if (!Pointer(klass) || !Pointer(outer) || outer == object ||
      !r.get(klass, cls) || !r.get(outer, package) ||
      (At<uint32_t>(cls.data() + 8) & 0x40000030) ||
      (At<uint32_t>(package.data() + 8) & 0x40000030))
    return {};
  std::string type, name, path;
  if (!NativeName(r, At<uint32_t>(cls.data() + 0x18),
                  At<uint32_t>(cls.data() + 0x1c), type) ||
      (type != "Texture2D" && type != "PaperSprite") ||
      !NativeName(r, At<uint32_t>(header.data() + 0x18),
                  At<uint32_t>(header.data() + 0x1c), name) ||
      !NativeName(r, At<uint32_t>(package.data() + 0x18),
                  At<uint32_t>(package.data() + 0x1c), path) ||
      !path.starts_with("/Game/") || !path.ends_with("/" + name) ||
      path.find("..") != std::string::npos ||
      name.find('/') != std::string::npos)
    return {};
  if (!r.get(object, checked) || checked != header || !r.get(klass, checked) ||
      checked != cls || !r.get(outer, checked) || checked != package)
    return {};
  asset_class = type;
  return path + "." + name;
}
ActorBattleStatuses Statuses(const Reader &r, uint64_t actor, uint32_t id,
                             const BattleMetadata &metadata) {
  ActorBattleStatuses out{};
  out.id = id;
  StatusSample a{}, b{};
  if (!SampleStatuses(r, actor, a) || !SampleStatuses(r, actor, b) || a != b)
    return out;
  unsigned active{};
  for (const auto &row : a.rows)
    if (At<uint32_t>(row.data()) && row[0xe0] == 0)
      ++active;
  // A never-built visible cache must not erase a nonempty current ailment list.
  if (active && a.icons.empty()) {
    out.unknown_count = active;
    return out;
  }
  out.valid = true;
  for (const auto &icon : a.icons) {
    uint32_t text = At<uint32_t>(icon.data());
    int32_t counter = At<int32_t>(icon.data() + 16);
    uint8_t mode = icon[20], category = icon[21];
    uint64_t texture = At<uint64_t>(icon.data() + 8);
    // Support-ability captions use another native resolver; Text0 is not a
    // name.
    if (!text || category == 1 || category > 2 || mode > 1 ||
        !Pointer(texture)) {
      ++out.unknown_count;
      continue;
    }
    const auto name = metadata.ui_texts.names.find(text);
    if (name == metadata.ui_texts.names.end() || name->second.empty()) {
      ++out.unknown_count;
      continue;
    }
    const BattleAilmentInfo *first = nullptr;
    uint32_t type{};
    bool unique = true, classification = true, counter_match = false;
    for (const auto &row : a.rows) {
      if (row[0xe0] != 0)
        continue;
      uint32_t candidate = At<uint32_t>(row.data());
      auto info = metadata.FindAilment(candidate);
      if (!info || info->text_id != text)
        continue;
      if (!first) {
        first = info;
        type = candidate;
      } else {
        unique = unique && type == candidate;
        classification = classification && first->positive == info->positive &&
                         first->negative == info->negative;
      }
      // Native3b7b210 passes this signed16 field to its visible icon counter.
      if (At<int16_t>(row.data() + 6) == counter)
        counter_match = true;
    }
    if (!first) {
      ++out.unknown_count;
      continue;
    }
    VisibleBattleStatus entry{};
    entry.type_id = unique ? type : 0;
    entry.text_id = text;
    entry.name = name->second;
    entry.counter = counter;
    entry.counter_mode = mode;
    entry.category = category;
    entry.counter_valid =
        mode == 0 && counter >= 0 && counter <= 32767 && counter_match;
    entry.positive = first->positive;
    entry.negative = first->negative;
    entry.classification_valid = classification;
    entry.texture_path = TexturePath(r, texture, entry.asset_class);
    out.entries.push_back(std::move(entry));
  }
  return out;
}
} // namespace
BattleStatusSnapshot ReadBattleStatuses(const ReadMemory &read, uint64_t main,
                                        const BattleSnapshot &battle,
                                        const BattleMetadata &metadata,
                                        int64_t delta) {
  BattleStatusSnapshot out{};
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !battle.valid ||
      !metadata.valid || battle.member_count < 1 || battle.member_count > 8 ||
      battle.enemy_count < 1 || battle.enemy_count > 6)
    return out;
  Reader r{read, main, delta};
  BoundActors actors{};
  if (!BindActors(r, battle, actors))
    return out;
  for (int i = 0; i < battle.member_count; ++i)
    out.party[i] = Statuses(r, actors.party[i], battle.members[i].id, metadata);
  for (int i = 0; i < battle.enemy_count; ++i)
    out.enemies[i] = Statuses(r, actors.context.enemies[i],
                              battle.enemies[i].enemy_id, metadata);
  if (!SameActors(r, battle, actors))
    return {};
  out.valid = true;
  return out;
}
} // namespace ot0
