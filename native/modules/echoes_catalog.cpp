// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_catalog.h"
#include "acnh_bytes.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace echoes {
namespace {
constexpr size_t Budget = 1024 * 1024;
constexpr std::string_view NamePrefix = "glossary/CopyableActor:";
bool String(std::span<const uint8_t> bytes, uint64_t start, uint64_t length,
            uint64_t pool, std::string& out) {
  if (start < pool || length > 256 || !acnh::bytes::Fits(bytes, start, length + 1) ||
      bytes[static_cast<size_t>(start + length)] != 0) return false;
  const auto* data = reinterpret_cast<const char*>(bytes.data() + start);
  for (uint64_t i = 0; i < length; ++i) {
    const auto c = static_cast<uint8_t>(data[i]);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '/' || c == ':')) return false;
  }
  out.assign(data, static_cast<size_t>(length));
  return true;
}
using Messages = std::unordered_map<std::string, std::string>;
bool ParseMessages(std::span<const uint8_t> b, Messages& out) {
  using namespace acnh::bytes;
  out.clear();
  if (b.size() < 32 || b.size() > Budget || std::memcmp(b.data(), "MsgStdBn", 8) ||
      b[8] != 0xff || b[9] != 0xfe || b[12] != 0 || b[13] != 3 ||
      Le32(b, 0x12) != b.size()) return false; // Native UTF-8 MSBT v3.
  const uint16_t sections = Le16(b, 14);
  if (!sections || sections > 16) return false;
  std::span<const uint8_t> labels, texts;
  size_t at = 32;
  for (uint16_t section = 0; section < sections; ++section) {
    if (!Fits(b, at, 16)) return false;
    const size_t size = Le32(b, at + 4), body = at + 16;
    if (!Fits(b, body, size)) return false;
    if (!std::memcmp(b.data() + at, "LBL1", 4)) {
      if (!labels.empty()) return false;
      labels = b.subspan(body, size);
    } else if (!std::memcmp(b.data() + at, "TXT2", 4)) {
      if (!texts.empty()) return false;
      texts = b.subspan(body, size);
    }
    at = body + ((size + 15) & ~size_t{15});
    if (at > b.size()) return false;
  }
  if (labels.size() < 4 || texts.size() < 4) return false;
  const size_t count = Le32(texts, 0), buckets = Le32(labels, 0);
  if (!count || count > 4096 || !buckets || buckets > 1024 ||
      !Fits(texts, 4, count * 4) || !Fits(labels, 4, buckets * 8)) return false;
  std::vector<std::string> decoded;
  for (size_t i = 0; i < count; ++i) {
    const size_t start = Le32(texts, 4 + i * 4);
    const size_t end = i + 1 == count ? texts.size() : Le32(texts, 8 + i * 4);
    if (start < 4 + count * 4 || start >= end || end > texts.size()) return false;
    std::string text;
    bool terminated = false;
    for (size_t p = start; p < end;) {
      const uint8_t c = texts[p];
      if (!c) { terminated = true; break; }
      if (c == 0x0e || c == 0x0f) {
        const size_t header = c == 0x0e ? 7 : 5;
        if (header > end - p) return false;
        const size_t size = c == 0x0e ? Le16(texts, p + 5) : 0;
        if (size > end - p - header) return false;
        p += header + size;
      } else {
        text.push_back(static_cast<char>(c));
        ++p;
      }
      if (text.size() > 8192) return false;
    }
    if (!terminated) return false;
    decoded.push_back(std::move(text));
  }
  Messages parsed;
  for (size_t i = 0; i < buckets; ++i) {
    const size_t n = Le32(labels, 4 + i * 8);
    size_t p = Le32(labels, 8 + i * 8);
    if (n > count || (n && p < 4 + buckets * 8)) return false;
    for (size_t j = 0; j < n; ++j) {
      if (!Fits(labels, p, 1)) return false;
      const size_t length = labels[p];
      if (!length || !Fits(labels, p + 1, length + 4)) return false;
      const size_t index = Le32(labels, p + 1 + length);
      if (index >= decoded.size()) return false;
      std::string name(reinterpret_cast<const char*>(labels.data() + p + 1), length);
      if (name.find('\0') != std::string::npos ||
          !parsed.emplace(std::move(name), decoded[index]).second) return false;
      p += 1 + length + 4;
    }
  }
  if (parsed.empty()) return false;
  out = std::move(parsed);
  return true;
}
} // namespace
uint32_t ActorKey(std::string_view actor) {
  uint32_t crc = UINT32_MAX;
  for (const char c : actor) {
    crc ^= static_cast<uint8_t>(c);
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
  }
  return ~crc;
}
bool DecodeNativeMessages(std::span<const uint8_t> bytes,
                          std::unordered_map<std::string, std::string>& out) {
  return ParseMessages(bytes, out);
}
bool ParseEchoMetadata(std::span<const uint8_t> b, std::vector<EchoMetadata>& out) {
  out.clear();
  using namespace acnh::bytes;
  if (b.size() < 0x30 || b.size() > Budget || std::memcmp(b.data(), "gsht", 4) ||
      Le32(b, 4) != 2) return false;
  const uint64_t pool = Le64(b, 0x10), rows = Le64(b, 0x20);
  const uint32_t count = Le32(b, 0x28), stride = Le32(b, 0x2c);
  // This module supports only the inspected v2 CopyableActor schema, not
  // arbitrary GSHTs sharing its magic. Verify field names/types/offsets below.
  if (count != 128 || stride != 0x60 || rows != 0x420 ||
      pool != rows + uint64_t(count) * stride || pool >= b.size() ||
      !Fits(b, rows, uint64_t(count) * stride)) return false;
  struct Field { size_t index; std::string_view name, type; uint16_t offset; };
  const std::array<Field, 7> fields{{
      {0, "mSymbol", "string", 0},
      {1, "mUi", "__inline_struct_CopyableActor_mUi", 0x10},
      {2, "mPasteCost", "__inline_struct_CopyableActor_mPasteCost", 0x28},
      {7, "mNameLabel", "string", 0},
      {8, "mCategory", "CopyableActorCategory", 0x10},
      {9, "mSlot", "int", 0},
      {10, "mReductionGroup", "PasteCostReduction", 4},
  }};
  for (const auto& f : fields) {
    const size_t at = 0x30 + f.index * 0x30;
    const uint64_t name = Le64(b, at), type = Le64(b, at + 8);
    std::string actual_name, actual_type;
    if (!String(b, name, f.name.size(), pool, actual_name) || actual_name != f.name ||
        !String(b, type, f.type.size(), pool, actual_type) || actual_type != f.type ||
        Le16(b, at + 20) != f.offset) return false;
  }
  std::vector<EchoMetadata> parsed;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = static_cast<size_t>(rows) + size_t(i) * stride;
    EchoMetadata entry;
    std::string reference;
    if (!String(b, Le64(b, at), Le64(b, at + 8), pool, entry.actor) ||
        entry.actor.empty() ||
        !String(b, Le64(b, at + 16), Le64(b, at + 24), pool, reference) ||
        !reference.starts_with(NamePrefix)) return false;
    entry.name_label = reference.substr(NamePrefix.size());
    entry.crc_key = ActorKey(entry.actor);
    entry.base_slots = Le32(b, at + 0x28);
    entry.reduction_group = Le32(b, at + 0x2c);
    if (entry.base_slots > 64 || entry.reduction_group > 64 ||
        std::any_of(parsed.begin(), parsed.end(), [&](const auto& old) {
          return old.crc_key == entry.crc_key;
        })) return false;
    parsed.push_back(std::move(entry));
  }
  out = std::move(parsed);
  return true;
}
bool ParseAutomatonMetadata(std::span<const uint8_t> b,
                            std::vector<EchoMetadata>& out) {
  out.clear();
  using namespace acnh::bytes;
  constexpr uint64_t rows=0x1b0,pool=0x360;
  if(b.size()<pool || b.size()>Budget || std::memcmp(b.data(),"gsht",4) ||
     Le32(b,4)!=2 || Le64(b,0x10)!=pool || Le64(b,0x20)!=rows ||
     Le32(b,0x28)!=6 || Le32(b,0x2c)!=0x48)return false;
  const std::array<std::string_view,8> fields{"mSymbol","mPriority","mNameLabel",
    "mFlavorTextLabel","mRepairPrice","mRepairDiscount","mRepairPriceCoupon",
    "mRepairDiscountCoupon"};
  const std::array<uint16_t,8> offsets{0,0x10,0x18,0x28,0x38,0x3c,0x40,0x44};
  for(size_t i=0;i<fields.size();++i) {
    const auto at=0x30+i*0x30;std::string name,type;
    const std::string_view expected=(i==0 || i==2 || i==3)?"string":"int";
    if(!String(b,Le64(b,at),fields[i].size(),pool,name) || name!=fields[i] ||
       !String(b,Le64(b,at+8),expected.size(),pool,type) || type!=expected ||
       Le16(b,at+20)!=offsets[i])return false;
  }
  std::vector<EchoMetadata> parsed;
  constexpr std::string_view description_prefix="ui/DictionaryExplanation:";
  for(size_t i=0;i<6;++i) {
    const auto at=rows+i*0x48;EchoMetadata entry;std::string name,description;
    if(!String(b,Le64(b,at),Le64(b,at+8),pool,entry.actor) || entry.actor.empty() ||
       !String(b,Le64(b,at+0x18),Le64(b,at+0x20),pool,name) ||
       !name.starts_with(NamePrefix) || name.size()==NamePrefix.size() ||
       !String(b,Le64(b,at+0x28),Le64(b,at+0x30),pool,description) ||
       !description.starts_with(description_prefix) || description.size()==description_prefix.size())return false;
    entry.crc_key=ActorKey(entry.actor);
    if(std::any_of(parsed.begin(),parsed.end(),[&](const auto& old){return old.crc_key==entry.crc_key;}))return false;
    entry.name_label=name.substr(NamePrefix.size());
    entry.description_label=description.substr(description_prefix.size());
    parsed.push_back(std::move(entry));
  }
  out=std::move(parsed);return true;
}
bool AttachEchoUiMetadata(std::span<const uint8_t> b,
                         std::vector<EchoMetadata>& entries) {
  using namespace acnh::bytes;
  for (auto& e : entries) e.description_label.clear();
  if (b.size() < 0x30 || b.size() > Budget || std::memcmp(b.data(), "gsht", 4) ||
      Le32(b, 4) != 2 || Le64(b, 0x20) != 0xc0 || Le32(b, 0x28) != 127 ||
      Le32(b, 0x2c) != 0x28 || Le64(b, 0x10) != 0x14a0 || b.size() <= 0x14a0)
    return false;
  const std::array<std::string_view, 3> fields{"mSymbol", "mPriority", "mFlavorTextLabel"};
  const std::array<uint16_t, 3> offsets{0, 0x10, 0x18};
  for (size_t i = 0; i < fields.size(); ++i) {
    const size_t at = 0x30 + i * 0x30;
    std::string name, type;
    const std::string_view expected_type = i == 1 ? "int" : "string";
    if (!String(b, Le64(b, at), fields[i].size(), 0x14a0, name) || name != fields[i] ||
        !String(b, Le64(b, at + 8), expected_type.size(), 0x14a0, type) ||
        type != expected_type || Le16(b, at + 20) != offsets[i]) return false;
  }
  auto updated = entries;
  std::vector<uint32_t> seen;
  constexpr std::string_view prefix = "ui/DictionaryExplanation:";
  for (size_t i = 0; i < 127; ++i) {
    const size_t at = 0xc0 + i * 0x28;
    std::string actor, reference;
    if (!String(b, Le64(b, at), Le64(b, at + 8), 0x14a0, actor) || actor.empty() ||
        !String(b, Le64(b, at + 24), Le64(b, at + 32), 0x14a0, reference) ||
        !reference.starts_with(prefix) || reference.size() == prefix.size()) return false;
    const auto key = ActorKey(actor);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return false;
    seen.push_back(key);
    const auto e = std::find_if(updated.begin(), updated.end(),
                             [&](const auto& entry) { return entry.actor == actor; });
    if (e == updated.end()) return false;
    e->description_label = reference.substr(prefix.size());
  }
  if (std::any_of(updated.begin(), updated.end(), [](const auto& e) {
        return !e.name_label.empty() && e.description_label.empty();
      })) return false;
  entries = std::move(updated);
  return true;
}
bool LocalizeEchoMetadata(std::vector<EchoMetadata>& entries,
                         const std::vector<uint8_t>& names_bytes,
                         const std::vector<uint8_t>& descriptions_bytes) {
  for (auto& entry : entries) { entry.name.clear(); entry.description.clear(); }
  if (names_bytes.size() > Budget || descriptions_bytes.size() > Budget) return false;
  Messages names, descriptions;
  if (!ParseMessages(names_bytes, names) || !ParseMessages(descriptions_bytes, descriptions)) return false;
  auto localized = entries;
  for (auto& entry : localized) {
    if (entry.name_label.empty()) continue; // Native unused/unlearned row.
    const auto name = names.find(entry.name_label), description = descriptions.find(entry.description_label);
    if (name == names.end() || description == descriptions.end() ||
        name->second.empty() || description->second.empty()) return false;
    entry.name = name->second;
    entry.description = description->second;
  }
  entries = std::move(localized);
  return true;
}
} // namespace echoes
