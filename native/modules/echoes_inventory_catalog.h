// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_reader.h"
#include <string>
#include <vector>
namespace echoes {
using InventoryReadAssetFn =
    std::function<size_t(std::string_view, uint64_t, void *, size_t)>;
enum class InventoryKind { Accessory, Outfit, Ingredient, Coupon };
struct InventoryMetadata {
  InventoryKind kind{};
  uint32_t mask{};        // Gear only: one verified native ownership bit.
  int quantity_index{-1}; // Ingredients only: index in the native 16-byte array.
  uint32_t item_id{};     // Items.gsheet ID when available; zero for UI gear.
  bool edible{}, drink{}; // Original UI list health effect and native Drink label branch.
  int32_t heal{};         // Edible only: quarter-hearts, truncated like the native eat (fcvtzs).
  std::string symbol, actor, texture, name, description;
};
struct InventoryCatalog {
  std::vector<InventoryMetadata> entries;
};
// Asset-worker operation. Reads original GSHT/MSBTs plus authenticated native
// pure dispatch bytes; interprets only whitelisted arithmetic/branches without
// executing guest code or making guest calls. No copied CRC/mask/name/actor
// table is embedded.
// message_root is the validated native locale root, e.g. regionUS/USen.
// Gear uses native UI icon symbols; its actor field is empty (unproven join).
// All failures clear out. This metadata does not establish player ownership.
bool LoadInventoryCatalog(const InventoryReadAssetFn &read_asset,
                          const ReadFn &read, uint64_t main_base,
                          bool build_matches, std::string_view message_root,
                          InventoryCatalog &out);
} // namespace echoes
