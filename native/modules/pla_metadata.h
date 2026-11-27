// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// ROMFS FlatBuffers layout: pkNX PokeInfoList.fbs (MIT), commit 3bc649ee.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pla {
class FlatMetadata {
  std::span<const uint8_t> bytes;
  bool ok;

public:
  explicit FlatMetadata(std::span<const uint8_t> data)
      : bytes(data), ok(data.size() >= 8 && data.size() <= 1024 * 1024) {}
  bool Valid() const { return ok; }
  uint64_t Read(size_t at, size_t size) {
    if (!ok || size > 8 || at > bytes.size() || size > bytes.size() - at) {
      ok = false;
      return 0;
    }
    uint64_t value = 0;
    for (size_t j = 0; j < size; ++j)
      value |= uint64_t(bytes[at + j]) << (8 * j);
    return value;
  }
  size_t Field(size_t table, unsigned index) {
    const int64_t vt = int64_t(table) - int32_t(Read(table, 4));
    if (!ok || vt < 0 || uint64_t(vt) > bytes.size()) {
      ok = false;
      return 0;
    }
    const auto size = Read(size_t(vt), 2), obj = Read(size_t(vt) + 2, 2);
    if (!ok || size < 4 || size % 2 || size > bytes.size() - size_t(vt) ||
        obj < 4 || table > bytes.size() || obj > bytes.size() - table) {
      ok = false;
      return 0;
    }
    if (index >= (size - 4) / 2)
      return 0;
    const auto offset = Read(size_t(vt) + 4 + 2 * index, 2);
    if (!offset)
      return 0;
    if (offset < 4 || offset >= obj) {
      ok = false;
      return 0;
    }
    return table + offset;
  }
  uint64_t Scalar(size_t table, unsigned index, size_t size) {
    const auto at = Field(table, index);
    if (!at)
      return 0;
    const auto value = Read(at, size);
    // Scalar fields must also fit inside this table, not just the input.
    const int64_t vt = int64_t(table) - int32_t(Read(table, 4));
    if (!ok || vt < 0) {
      ok = false;
      return 0;
    }
    const auto obj = Read(size_t(vt) + 2, 2);
    if (at - table + size > obj)
      ok = false;
    return value;
  }
  size_t Target(size_t table, unsigned index) {
    const auto at = Field(table, index);
    if (!at) {
      ok = false;
      return 0;
    }
    const auto offset = Scalar(table, index, 4);
    if (!ok || offset < 4 || offset > bytes.size() - at) {
      ok = false;
      return 0;
    }
    return at + offset;
  }
  std::vector<size_t> Tables(size_t table, unsigned index, unsigned limit) {
    const auto at = Target(table, index);
    const auto count = Read(at, 4);
    if (!ok || count > limit || at > bytes.size() - 4 ||
        count > (bytes.size() - at - 4) / 4) {
      ok = false;
      return {};
    }
    std::vector<size_t> result;
    for (unsigned j = 0; j < count; ++j) {
      const auto cell = at + 4 + 4 * j;
      const auto offset = Read(cell, 4);
      if (offset < 4 || offset > bytes.size() - cell) {
        ok = false;
        return {};
      }
      result.push_back(cell + offset);
    }
    return result;
  }
  std::vector<uint32_t> U32Values(size_t table, unsigned index, unsigned limit) {
    const auto at = Target(table, index);
    const auto count = Read(at, 4);
    if (!ok || count > limit || at > bytes.size() - 4 ||
        count > (bytes.size() - at - 4) / 4) {
      ok = false;
      return {};
    }
    std::vector<uint32_t> result;
    for (unsigned j = 0; j < count; ++j)
      result.push_back(uint32_t(Read(at + 4 + 4 * j, 4)));
    return result;
  }
  std::string Asset(size_t table, unsigned index) {
    const auto at = Target(table, index);
    const auto count = Read(at, 4);
    if (!ok || count == 0 || count > 128 || at > bytes.size() - 4 ||
        count >= bytes.size() - at - 4 || Read(at + 4 + count, 1)) {
      ok = false;
      return {};
    }
    std::string result;
    for (size_t j = 0; j < count; ++j) {
      const auto c = Read(at + 4 + j, 1);
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_')) {
        ok = false;
        return {};
      }
      result += char(c);
    }
    return result;
  }
};
// Exact 1.1.1 res_item/list/item_info_list.bin: ID plus native asset stem.
// Several key items share artwork; an item ID is not an icon filename.
inline std::string ItemIcon(std::span<const uint8_t> bytes, unsigned id) {
  if (!id || id > 4095) return {};
  FlatMetadata f(bytes);
  const auto tables = f.Tables(f.Read(0, 4), 1, 1024);
  if (!f.Valid() || tables.size() != 806) return {};
  std::string asset;
  unsigned matches = 0;
  for (const auto table : tables) {
    if (f.Scalar(table, 0, 4) != id) continue;
    asset = f.Asset(table, 1);
    ++matches;
  }
  if (!f.Valid() || matches != 1 || !asset.starts_with("item_")) return {};
  return "bin/appli/res_item/icon_item/" + asset + ".bntx";
}
inline std::string PokemonIcon(std::span<const uint8_t> bytes, unsigned species,
                               unsigned form, unsigned gender, bool shiny) {
  if (!species || species > 905 || form > 255 || gender > 2)
    return {};
  FlatMetadata f(bytes);
  const auto root = f.Read(0, 4);
  std::string match;
  unsigned species_matches = 0, form_matches = 0, asset_matches = 0;
  for (const auto s : f.Tables(root, 1, 1024)) {
    if (f.Scalar(s, 0, 4) != species)
      continue;
    ++species_matches;
    for (const auto p : f.Tables(s, 1, 256)) {
      if (f.Scalar(p, 0, 4) != form)
        continue;
      ++form_matches;
      for (const auto r : f.Tables(p, 1, 2)) {
        const auto rare = f.Scalar(r, 0, 1);
        if (rare > 1)
          return {};
        if (bool(rare) != shiny)
          continue;
        for (const auto u : f.Tables(r, 1, 2)) {
          // Unidentified second variant: do not guess it.
          if (f.Scalar(u, 0, 1) != 0)
            continue;
          for (const auto a : f.Tables(u, 1, 8)) {
            const auto g = f.Scalar(f.Target(a, 0), 0, 1);
            const bool fits = gender == 0   ? (g == 0 || g == 3 || g == 4)
                              : gender == 1 ? (g == 1 || g == 3 || g == 5)
                                            : g == 2;
            if (!fits)
              continue;
            match = f.Asset(a, 1);
            ++asset_matches;
          }
        }
      }
    }
  }
  if (!f.Valid() || species_matches != 1 || form_matches != 1 ||
      asset_matches != 1)
    return {};
  return "bin/appli/res_pokemon/icon_pokemon_s/pokeicon_s_" + match + ".bntx";
}
} // namespace pla
