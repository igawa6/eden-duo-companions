// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_inventory.h"
#include "echoes_code_guard.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x80368800, Owner = 0x210bd9a000, State = 0x210bd9d000;
size_t cases{};
void Check(bool ok, const char* text) {
  if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
  }
  Memory() {
    for (const auto& g : echoes::InventoryIdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i) bytes[Base + g.offset + i] = g.bytes[i];
    for (const auto& g : echoes::RupeeIdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i) bytes[Base + g.offset + i] = g.bytes[i];
    constexpr uint64_t methods[]{0x2127cbc,0x2127d10,0x2127d60,0x2127da0,
      0x2127ed0,0x2128020,0x2128100,0x21281f0,0x2128290,0x21283d0,0x2128520,0x2128610};
    for (size_t i = 0; i < 12; ++i) Put(Base + 0x33cdda0 + i * 8, Base + methods[i], 8);
    Put(Base + 0x34bc840, Owner, 8);
    for (size_t i = 0; i < 32; ++i) bytes[Owner + i] = bytes[State + i] = 0;
    Put(Owner, Base + 0x33cdda0, 8);
    Put(Owner + 8, State, 8);
    // Raw independently observed loaded-field counter from context-10-field.
    Put(Owner + 0x10, UINT64_C(0x0409600700000000), 8);
    Put(Owner + 0x18, 0x86225110, 8);
    Put(State + 0x17, 0x28, 1);
    Put(State + 0x1858, 1234, 4); // Synthetic quantity, not a live-device claim.
    for (size_t i = 0; i < 0x37; ++i) bytes[State + 0x185d + i] = 0;
    Put(State + 0x1874, 33554431, 4);
    Put(State + 0x1878, 2047, 2);
    Put(State + 0x187a, 32, 2);
    Put(State + 0x187c, 268426751, 4);
    constexpr uint32_t equipped[]{2,64,4096,16384,134217728};
    for (size_t i = 0; i < 5; ++i) Put(State + 0x1880 + i * 4, equipped[i], 4);
    constexpr uint8_t quantities[]{16,40,42,55,52,37,39,43,36,47,48,44,33,31,36,26};
    for (size_t i = 0; i < 16; ++i) Put(State + 0x185e + i, quantities[i], 1);
    Put(State + 0x185d, 35, 1);
  }
  bool Read(uint64_t at, void* out, size_t n) {
    ++reads; largest = std::max(largest, n);
    size_t visit = ++visits[at];
    if (before) before(at, visit);
    if (!at || n > std::numeric_limits<uint64_t>::max() - at) return false;
    for (size_t i = 0; i < n; ++i) {
      const auto p = bytes.find(at + i);
      if (p == bytes.end()) return false;
      static_cast<uint8_t*>(out)[i] = p->second;
    }
    return true;
  }
  echoes::InventorySnapshot Sample(bool build = true, uint64_t base = Base) {
    return echoes::ReadInventory([&](auto a, auto p, auto n) { return Read(a,p,n); }, base, build);
  }
  echoes::RupeeSnapshot Rupees(bool build = true, uint64_t base = Base) {
    return echoes::ReadRupees([&](auto a, auto p, auto n) { return Read(a,p,n); },
                             base, build);
  }
};
void Empty(const echoes::InventorySnapshot& s) {
  Check(!s.inventory_valid && s.source == echoes::InventorySource{} &&
        !s.acquired_accessories && !s.acquired_outfits && !s.equipped_outfit &&
        !s.accessory_capacity && !s.equipped_accessory_count && !s.coupons &&
        std::all_of(s.ingredients.begin(),s.ingredients.end(),[](auto x){return x==0;}) &&
        std::all_of(s.equipped_accessories.begin(),s.equipped_accessories.end(),[](auto x){return x==0;}),
        "invalid sample clears every inventory field");
}
void Reject(const std::function<void(Memory&)>& change) {
  Memory m; change(m); Empty(m.Sample()); ++cases;
}
void NoRupees(const echoes::RupeeSnapshot& sample) {
  Check(!sample.rupees_valid && sample.rupees == 0 &&
        sample.source == echoes::InventorySource{}, "invalid rupees clear value/source");
}
void RupeeReject(const std::function<void(Memory&)>& change) {
  Memory m; change(m); NoRupees(m.Rupees()); ++cases;
}
} // namespace

int main() {
  {
    Memory m; const auto s = m.Sample();
    Check(s.compatible && s.native_predicate_supported && s.inventory_valid &&
          s.source.owner == Owner && s.source.state == State,
          "captured field values pass stable conservative native predicate");
    Check(s.acquired_accessories == 268426751 && s.acquired_outfits == 2047 &&
          s.equipped_outfit == 32 && s.accessory_capacity == 5 &&
          s.equipped_accessory_count == 5 && s.equipped_accessories[4] == 134217728 &&
          s.coupons == 35 && s.ingredients[0] == 16 && s.ingredients[15] == 26,
          "preserves native ownership, equipped masks and noncontiguous ingredient15");
    Check(m.largest <= 256 && m.reads < 30, "fixed spans and bounded read count");
    m.Put(Base + 0x34bc840, 0, 8); Empty(m.Sample()); ++cases;
  }
  {
    Memory m; Check(!m.Sample(false).compatible && m.reads==0, "wrong build reads nothing");
    Check(!m.Sample(true,0).compatible, "zero base");
    Check(!m.Sample(true,Base+1).compatible, "unaligned base");
    Check(!m.Sample(true,UINT64_MAX-3).compatible, "base overflow");
    Empty(echoes::ReadInventory({},Base,true)); ++cases;
  }
  Reject([](auto& m){m.bytes[Base+0x2128610]^=1;});
  Reject([](auto& m){m.bytes.erase(Base+0x2501cc0);});
  Reject([](auto& m){m.Put(Base+0x34bc840,Owner+1,8);});
  Reject([](auto& m){m.Put(Owner+8,State+1,8);});
  Reject([](auto& m){m.Put(Owner,Base+0x33cdda8,8);});
  Reject([](auto& m){m.Put(Base+0x33cdda0+0x58,Base+0x2128520,8);});
  Reject([](auto& m){m.bytes.erase(State+0x1884);});
  for (const uint64_t counter : {UINT64_C(0),UINT64_C(0x0409600700000001),
       UINT64_C(0x0509600700000000),UINT64_C(0x0609600700000000),
       UINT64_C(0x0409600701000000),UINT64_C(0x0409600000000000)})
    Reject([counter](auto& m){m.Put(Owner+0x10,counter,8);});
  Reject([](auto& m){m.Put(State+0x17,0x68,1);});
  Reject([](auto& m){m.Put(State+0x1878,0x8000,2);});
  Reject([](auto& m){m.Put(State+0x187c,0x80000000,4);});
  Reject([](auto& m){m.Put(State+0x187a,3,2);});
  Reject([](auto& m){m.Put(State+0x187a,0,2);});
  Reject([](auto& m){m.Put(State+0x1878,1,2);});
  Reject([](auto& m){m.Put(State+0x1884,3,4);});
  Reject([](auto& m){m.Put(State+0x1884,2,4);});
  Reject([](auto& m){m.Put(State+0x1884,0x10000000,4);});
  Reject([](auto& m){m.Put(State+0x187c,1,4);});
  Reject([](auto& m){m.Put(State+0x185d,100,1);});
  Reject([](auto& m){m.Put(State+0x186d,255,1);});
  {
    Memory m;
    m.Put(State+0x1874,0,4); m.Put(State+0x1880,0,4);
    // Slots outside current capacity are deliberately not presented.
    auto s=m.Sample();
    Check(s.inventory_valid && s.accessory_capacity==1 && !s.equipped_accessory_count &&
          s.equipped_accessories==std::array<uint32_t,5>{}, "one empty slot is valid; inactive slots cleared");
    ++cases;
  }
  for (const uint64_t address : {Base+0x34bc840,Owner,State,State+0x185d}) {
    Reject([address](auto& m){m.before=[&,address](auto at,auto visit){
      if(at==address && visit==2) m.bytes[address]^=1;
    };});
  }
  Reject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==State && visit==3)m.Put(Base+0x34bc840,Owner+0x1000,8);
  };});
  Reject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==State+0x185d && visit==2)m.Put(Owner+8,State+0x1000,8);
  };});
  Reject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==Owner && visit==3)m.Put(Owner+0x10,UINT64_C(0x0409600700000100),8);
  };});
  {
    Memory m;
    auto s = m.Rupees();
    Check(s.compatible && s.native_predicate_supported && s.rupees_valid &&
          s.rupees == 1234 && s.source.owner == Owner && s.source.state == State,
          "stable native rupee field retains source");
    Check(m.largest <= 256 && m.reads < 25 && m.visits[State+0x185d] == 0,
          "bounded rupee getter does not sample gear/ingredients");
    for (auto value : {uint32_t(0), uint32_t(9999)}) {
      m.Put(State+0x1858,value,4); s=m.Rupees();
      Check(s.rupees_valid && s.rupees==value, "native zero/cap are valid values");
    }
    m.Put(Base+0x34bc840,0,8); NoRupees(m.Rupees()); ++cases;
  }
  {
    Memory m; const auto s=m.Rupees(false);
    Check(!s.compatible && m.reads==0, "rupees reject wrong build without reads");
    NoRupees(m.Rupees(true,0)); NoRupees(m.Rupees(true,Base+1));
    NoRupees(m.Rupees(true,UINT64_MAX-3));
    NoRupees(echoes::ReadRupees({},Base,true)); ++cases;
  }
  {
    Memory m;
    // Rupees do not depend on valid equipment or even readable gear payload.
    for(size_t i=0;i<0x37;++i)m.bytes.erase(State+0x185d+i);
    Check(m.Rupees().rupees_valid && m.Rupees().rupees==1234,
          "gear validation cannot suppress an independently valid wallet"); ++cases;
  }
  for (auto value : {uint32_t(10000), uint32_t(0x80000000), UINT32_MAX})
    RupeeReject([value](auto& m){m.Put(State+0x1858,value,4);});
  for (const auto& guard : echoes::RupeeIdentityGuards)
    RupeeReject([&guard](auto& m){m.bytes[Base+guard.offset]^=1;});
  RupeeReject([](auto& m){m.bytes.erase(State+0x1858);});
  RupeeReject([](auto& m){m.Put(Base+0x34bc840,Owner+1,8);});
  RupeeReject([](auto& m){m.Put(Owner+8,0,8);});
  RupeeReject([](auto& m){m.Put(Owner,Base+0x33cdda8,8);});
  RupeeReject([](auto& m){m.Put(Base+0x33cdda0+0x58,Base+0x2128520,8);});
  RupeeReject([](auto& m){m.Put(State+0x17,0x68,1);});
  RupeeReject([](auto& m){m.Put(Owner+0x10,UINT64_C(0x0509600700000000),8);});
  RupeeReject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==State+0x1858 && visit==2)m.Put(State+0x1858,1235,4);
  };});
  RupeeReject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==State+0x1858 && visit==2)m.Put(Owner+8,State+0x1000,8);
  };});
  RupeeReject([](auto& m){m.before=[&](auto at,auto visit){
    if(at==State && visit==3)m.Put(Base+0x34bc840,Owner+0x1000,8);
  };});
  {
    Memory m; size_t ordered=0;
    for(const auto& guard:echoes::RupeeIdentityGuards) {
      for(size_t i=0;i<guard.bytes.size();i+=4) {
        const auto* p=guard.bytes.data()+i;
        const uint32_t word=uint32_t(p[0])|uint32_t(p[1])<<8|
                            uint32_t(p[2])<<16|uint32_t(p[3])<<24;
        if(echoes::OriginalExclusive(word)) {
          m.Put(Base+guard.offset+i,word|0x8000u,4); ++ordered;
        }
      }
    }
    Check(ordered>0 && m.Rupees().rupees_valid,
          "original NCE ordered-exclusive rewrite is accepted by rupee guards"); ++cases;
  }
  std::cout << cases << " inventory/rupee cases passed\n";
}
