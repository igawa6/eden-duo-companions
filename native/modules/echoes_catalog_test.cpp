// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_catalog.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
std::vector<uint8_t> File(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
}
int main() {
  assert(echoes::ActorKey("123456789") == 0xcbf43926);
  const auto* fixture = std::getenv("ECHOES_ROMFS");
  if (!fixture) {
    std::cout << "ECHOES_ROMFS absent: native metadata fixture check skipped\n";
    return 77;
  }
  const std::filesystem::path root(fixture);
  const auto bytes = File(root / "region_common/datasheets/CopyableActor.gsheet");
  const auto names = File(root / "regionUS/USen/message/glossary/CopyableActor.msbt");
  const auto ui = File(root / "region_common/datasheets/ui/UiActorSelect.gsheet");
  const auto descriptions = File(root / "regionUS/USen/message/ui/DictionaryExplanation.msbt");
  std::vector<echoes::EchoMetadata> metadata;
  assert(echoes::ParseEchoMetadata(bytes, metadata) && metadata.size() == 128);
  assert(echoes::AttachEchoUiMetadata(ui, metadata));
  assert(echoes::LocalizeEchoMetadata(metadata, names, descriptions));
  const auto machines = File(root / "region_common/datasheets/ui/UiMachineSelect.gsheet");
  std::vector<echoes::EchoMetadata> automata;
  assert(echoes::ParseAutomatonMetadata(machines,automata) && automata.size()==6);
  assert(echoes::LocalizeEchoMetadata(automata,names,descriptions));
  for(const auto& entry:automata) {
    assert(!entry.actor.empty() && !entry.name.empty() && !entry.description.empty());
    assert(entry.crc_key==echoes::ActorKey(entry.actor));
    assert(entry.name_label=="MachineActor_"+entry.actor);
    assert(entry.description_label=="Dictionary_"+entry.actor);
  }
  for(size_t size:{size_t(0),size_t(47),size_t(0x1b0),size_t(0x360)})
    assert(!echoes::ParseAutomatonMetadata(std::span(machines).first(size),automata) && automata.empty());
  for(size_t offset:{size_t(0),size_t(4),size_t(0x10),size_t(0x20),size_t(0x28),
      size_t(0x2c),size_t(0x30),size_t(0x30+20),size_t(0x1b0),size_t(0x1b8),
      size_t(0x1c8),size_t(0x1d8)}) {
    auto corrupt=machines;corrupt[offset]^=0xff;
    assert(!echoes::ParseAutomatonMetadata(corrupt,automata) && automata.empty());
  }
  auto repeated=machines;
  std::copy_n(machines.begin()+0x1b0,16,repeated.begin()+0x1b0+0x48);
  assert(!echoes::ParseAutomatonMetadata(repeated,automata) && automata.empty());
  const auto localized = std::count_if(metadata.begin(), metadata.end(),
                                      [](const auto& e) { return !e.name.empty(); });
  assert(localized == 127);
  const auto find = [&](std::string_view actor) -> const auto& {
    const auto e = std::find_if(metadata.begin(), metadata.end(),
                               [&](const auto& entry) { return entry.actor == actor; });
    assert(e != metadata.end());
    return *e;
  };
  assert(find("Pitty").name == "Holmill");
  assert(find("Keese00").name == "Keese" && !find("Keese00").description.empty());
  assert(find("SeaUrchin").name == "Sea Urchin" && !find("SeaUrchin").description.empty());
  assert(find("Octorok").description_label == "Dictionary_Test_Octorok");
  assert(find("Octorok01").description_label == "CopyableActor_Octorok01");
  assert(find("BedZelda").base_slots == 3);
  assert(find("Lynel").base_slots == 6);
  // Every incomplete header/schema/row/string is rejected without partial metadata.
  for (size_t size : {size_t{0}, size_t{47}, size_t{0x420}, size_t{0x3420}}) {
    assert(!echoes::ParseEchoMetadata(std::span(bytes).first(size), metadata));
    assert(metadata.empty());
  }
  for (size_t offset : {size_t{0},size_t{4},size_t{0x10},size_t{0x20},size_t{0x28},
                       size_t{0x2c},size_t{0x30},size_t{0x30+20},size_t{0x420},
                       size_t{0x428},size_t{0x430},size_t{0x438},size_t{0x448}}) {
    auto corrupt = bytes;
    corrupt[offset] ^= 0xff;
    assert(!echoes::ParseEchoMetadata(corrupt, metadata));
    assert(metadata.empty());
  }
  auto duplicate = bytes;
  std::copy_n(bytes.begin()+0x420,16,duplicate.begin()+0x480);
  assert(!echoes::ParseEchoMetadata(duplicate, metadata) && metadata.empty());
  assert(echoes::ParseEchoMetadata(bytes, metadata));
  assert(!echoes::LocalizeEchoMetadata(metadata, {}, descriptions));
  assert(std::all_of(metadata.begin(), metadata.end(), [](const auto& e) {
    return e.name.empty() && e.description.empty();
  }));
  std::cout << "Native Echo metadata:128records/127localized, aliases and malformed inputs passed\n";
}
