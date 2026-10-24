// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_inventory.h"
#include "echoes_prologue.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
namespace {
size_t checks{};
void Check(bool value, const char *reason) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL " << checks << ": " << reason << '\n';
    std::abort();
  }
}
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint64_t Q(const std::vector<uint8_t> &b, size_t p) {
  uint64_t v{};
  for (size_t i = 0; i < 8; ++i)
    v |= uint64_t(b[p + i]) << (i * 8);
  return v;
}
void Put(std::vector<uint8_t> &b, size_t p, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i)
    b[p + i] = uint8_t(v >> (i * 8));
}
constexpr uint64_t Base = 0x80000000, Player = 0x90000000, State = 0x90010000;
struct ActorMemory {
  std::map<uint64_t, uint8_t> data;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  void Set(uint64_t p, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i)
      data[p + i] = uint8_t(value >> (i * 8));
  }
  void Guard(const echoes::IdentityGuard &g) {
    for (size_t i = 0; i < g.bytes.size(); ++i)
      data[Base + g.offset + i] = g.bytes[i];
  }
  ActorMemory() {
    Guard(echoes::InventoryIdentityGuards[0]);
    for (const auto &g : echoes::PrologueActorGuards)
      Guard(g);
    constexpr std::array<uint64_t, 12> methods{
        {0x2127cbc, 0x2127d10, 0x2127d60, 0x2127da0, 0x2127ed0, 0x2128020,
         0x2128100, 0x21281f0, 0x2128290, 0x21283d0, 0x2128520, 0x2128610}};
    for (size_t i = 0; i < methods.size(); ++i)
      Set(Base + 0x33cdda0 + i * 8, Base + methods[i], 8);
    for (size_t i = 0; i < 32; ++i) {
      data[Player + i] = 0;
      data[State + i] = 0;
    }
    Set(Base + 0x34bc840, Player, 8);
    Set(Player, Base + 0x33cdda0, 8);
    Set(Player + 8, State, 8);
    Set(Player + 16, UINT64_C(0x0409600700000000), 8);
    Set(State + 0x17, 0x28, 1);
    Set(State + 0xb0, 0x394, 2);
    Set(State + 0x1848, 0, 4);
  }
  bool Read(uint64_t p, void *dst, size_t n) {
    if (before)
      before(p, ++visits[p]);
    for (size_t i = 0; i < n; ++i) {
      auto it = data.find(p + i);
      if (it == data.end())
        return false;
      static_cast<uint8_t *>(dst)[i] = it->second;
    }
    return true;
  }
  echoes::PrologueActorSnapshot Get(bool build = true) {
    return echoes::ReadPrologueActor(
        [&](auto p, auto dst, auto n) { return Read(p, dst, n); }, Base, build);
  }
};
void SwordfighterTests() {
  const auto get=[](ActorMemory& m,bool build=true) {
    return echoes::ReadSwordfighter([&](auto p,auto dst,auto n){return m.Read(p,dst,n);},Base,build);
  };
  ActorMemory m;m.Set(State+0xb0,0x395,2);
  Check(get(m).known && !get(m).active,"ordinary Zelda keeps Echo shortcuts");
  m.Set(State+0x1848,1,4);
  Check(get(m).known && get(m).active,"native mode one selects combat rail");
  ActorMemory link;link.Set(State+0x1848,1,4);
  Check(get(link).known && !get(link).active,"PrologueLink remains a separate opening scope");
  for(int fault=0;fault<7;++fault) {
    ActorMemory bad;bad.Set(State+0xb0,0x395,2);bad.Set(State+0x1848,1,4);
    if(fault==0)bad.Set(State+0x1848,2,4);
    if(fault==1)bad.data.erase(State+0x1848);
    if(fault==2)bad.Set(State+0x17,0x40,1);
    if(fault==3)bad.data[Base+0x24dcf00]^=1;
    if(fault==4)bad.before=[&](uint64_t p,size_t n){if(p==State+0x1848 && n==2)bad.Set(p,0,4);};
    if(fault==5)bad.before=[&](uint64_t p,size_t n){if(p==State+0x1848 && n==1)bad.Set(Base+0x34bc840,0,8);};
    Check(!get(bad,fault!=6).known,"unknown/torn/unsupported native mode cannot expose combat actions");
  }
}
void ActorTests() {
  ActorMemory m;
  auto out = m.Get();
  Check(out.compatible && out.known && out.link && out.kind == 0x394 &&
            out.owner == Player && out.state == State,
        "exact native PrologueLink model is known");
  for (const uint16_t kind :
       {uint16_t(0), uint16_t(0x393), uint16_t(0x395), uint16_t(0xffff)}) {
    ActorMemory z;
    z.Set(State + 0xb0, kind, 2);
    z.Set(State + 0x1848, 1, 4);
    out = z.Get();
    Check(out.known && !out.link && out.kind == kind,
          "Zelda/transformation/unknown kinds cannot authorize Link equipment");
  }
  for (int fault = 0; fault < 10; ++fault) {
    ActorMemory bad;
    if (fault == 0)
      bad.Set(Base + 0x34bc840, 0, 8);
    if (fault == 1)
      bad.Set(Player + 8, State + 1, 8);
    if (fault == 2)
      bad.Set(Player, Base + 0x33cdda8, 8);
    if (fault == 3)
      bad.Set(Player + 16, 1, 8);
    if (fault == 4)
      bad.Set(Player + 16, 0, 8);
    if (fault == 5)
      bad.Set(State + 0x17, 0x68, 1);
    if (fault == 6)
      bad.data.erase(State + 0xb1);
    if (fault == 7)
      bad.data[Base + echoes::PrologueActorGuards[0].offset] ^= 1;
    if (fault == 8)
      bad.data[Base + echoes::PrologueActorGuards[1].offset] ^= 1;
    if (fault == 9)
      bad.data[Base + 0x33cdda0] ^= 1;
    Check(!bad.Get().known,
          "unsupported code/type or inaccessible player fails closed");
  }
  for (int race = 0; race < 4; ++race) {
    ActorMemory bad;
    bad.before = [&](uint64_t p, size_t visit) {
      if (race == 0 && p == State + 0xb0 && visit == 2)
        bad.Set(p, 0x395, 2);
      if (race == 1 && p == State && visit == 3)
        bad.Set(State + 0x17, 0x68, 1);
      if (race == 2 && p == Player && visit == 3)
        bad.Set(Player + 8, State + 8, 8);
      if (race == 3 && p == Base + 0x34bc840 && visit == 3)
        bad.Set(p, Player + 8, 8);
    };
    Check(!bad.Get().known,
          "model/header/root transition withdraws Link identity");
  }
  Check(!m.Get(false).known, "build mismatch rejects current actor");
  ActorMemory ordered;
  for (const auto &g :
       {echoes::InventoryIdentityGuards[0], echoes::PrologueActorGuards[0],
        echoes::PrologueActorGuards[1]}) {
    for (size_t i = 0; i < g.bytes.size(); i += 4) {
      uint32_t w = uint32_t(g.bytes[i]) | uint32_t(g.bytes[i + 1]) << 8 |
                   uint32_t(g.bytes[i + 2]) << 16 |
                   uint32_t(g.bytes[i + 3]) << 24;
      if (echoes::OriginalExclusive(w))
        ordered.Set(Base + g.offset + i, w | 0x8000u, 4);
    }
  }
  Check(ordered.Get().link,
        "qualified NCE ordered exclusives retain Link source identity");
  if (const char *dir = std::getenv("ECHOES_SEGMENTS")) {
    const auto text = File(std::filesystem::path(dir) / "text.bin");
    for (const auto &g :
         {echoes::InventoryIdentityGuards[0], echoes::PrologueActorGuards[0],
          echoes::PrologueActorGuards[1]})
      Check(g.offset <= text.size() &&
                g.bytes.size() <= text.size() - g.offset &&
                std::equal(g.bytes.begin(), g.bytes.end(),
                           text.begin() + std::ptrdiff_t(g.offset)),
            "actor guards match retained original supported text");
  }
}
} // namespace
int main() {
  ActorTests();SwordfighterTests();
  size_t reads{};
  const echoes::ReadFn memory = [&](auto, auto, auto) {
    ++reads;
    return false;
  };
  Check(!echoes::ReadPrologueScope(memory, 0x80000000, false, {}, {}).opening &&
            !reads,
        "wrong build cannot observe an opening scene");
  Check(!echoes::ReadPrologueScope(memory, 0x80000000, true, {}, {}).opening &&
            !reads,
        "unqualified resource cannot identify opening from learned echo count");
  const char *env = std::getenv("ECHOES_ROMFS");
  if (!env) {
    std::cout << "Original RomFS unavailable\n";
    return 77;
  }
  const std::filesystem::path root(env);
  const std::string path = "region_common/datasheets/ui/UiPrologueMenu.gsheet";
  const auto original = File(root / path);
  std::map<std::string, std::vector<uint8_t>, std::less<>> files;
  const echoes::ReadAssetFn read = [&](std::string_view p, uint64_t off,
                                       void *out, size_t n) {
    auto it = files.find(p);
    if (it == files.end())
      it = files.emplace(std::string(p), File(root / std::string(p))).first;
    const auto &b = it->second;
    if (!out && !n)
      return b.size();
    if (off > b.size() || n > b.size() - off)
      return size_t(0);
    std::copy_n(b.data() + off, n, static_cast<uint8_t *>(out));
    return n;
  };
  std::vector<echoes::PrologueItem> rows;
  Check(echoes::LoadPrologueItems(read, "regionUS/USen", rows) &&
            rows.size() == 5,
        "all original prologue rows and native labels load");
  Check(rows[1].texture == "Sword_01" && rows[2].texture == "Item_Shield" &&
            rows[3].texture == "Arrow_01" && rows[4].texture == "Bomb_01" &&
            !rows[1].description.empty() && !rows[4].name.empty(),
        "native Link equipment artwork and prose retain original order");
  for (int fault = 0; fault < 7; ++fault) {
    auto b = original;
    const size_t first = size_t(Q(b, 32));
    if (fault == 0)
      b[4] = 3;
    if (fault == 1)
      Put(b, 40, 6, 4);
    if (fault == 2)
      Put(b, 44, 47, 4);
    if (fault == 3)
      Put(b, 48 + 20, 16, 2);
    if (fault == 4)
      Put(b, first + 8, UINT64_MAX, 8);
    if (fault == 5)
      std::copy_n(b.data() + first, 16, b.data() + first + 48);
    if (fault == 6)
      b.resize(first + 40);
    files[path] = std::move(b);
    Check(!echoes::LoadPrologueItems(read, "regionUS/USen", rows) &&
              rows.empty(),
          "malformed prologue metadata cannot expose partial gear rows");
  }
  files[path] = original;
  files["regionUS/USen/message/ui/ItemMenu.msbt"].clear();
  Check(!echoes::LoadPrologueItems(read, "regionUS/USen", rows) && rows.empty(),
        "missing native localized explanations withdraw passive inventory");
  std::cout << checks << " native prologue checks passed\n";
}
