// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace echoes {
struct EchoMetadata {
  uint32_t crc_key{};
  std::string actor, name_label, description_label, name, description;
  // The native datasheet's mPasteCost.mSlot and mReductionGroup. These do
  // not establish the current cost after the player's Tri-level reductions.
  uint32_t base_slots{}, reduction_group{};
};
uint32_t ActorKey(std::string_view actor);
// Native UTF-8 MSBT v3, including bounded control-tag skipping.
bool DecodeNativeMessages(std::span<const uint8_t> bytes,
                          std::unordered_map<std::string, std::string>& out);
// Reads original CopyableActor GSHT v2 metadata. Ownership/order is separate.
// No localized strings, native ID tables or artwork are embedded in the module.
bool ParseEchoMetadata(std::span<const uint8_t> bytes,
                       std::vector<EchoMetadata>& out);
// Original UiMachineSelect schema; supplies the six Automaton symbols and
// localized name/description references. Ownership/order remains separate.
bool ParseAutomatonMetadata(std::span<const uint8_t> bytes,
                            std::vector<EchoMetadata>& out);
// The native UiActorSelect table supplies description aliases explicitly.
bool AttachEchoUiMetadata(std::span<const uint8_t> bytes,
                         std::vector<EchoMetadata>& entries);
bool LocalizeEchoMetadata(std::vector<EchoMetadata>& entries,
                         const std::vector<uint8_t>& names_msbt,
                         const std::vector<uint8_t>& descriptions_msbt);
} // namespace echoes
