// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
using namespace echoes;
std::array<uint8_t,4> Bytes(uint32_t w) {
  return {uint8_t(w),uint8_t(w>>8),uint8_t(w>>16),uint8_t(w>>24)};
}
int main() {
  size_t checks=0;
  const auto check=[&](bool value){++checks;assert(value);};
  // Real runtime static fixtures plus native EoW LDXR/STXR words. All four
  // instruction families are compared in explicit little-endian byte order.
  for(uint32_t word:{0xc85f7c00u,0xc8007c00u,0xc87f0440u,0xc8200440u,
                     0xc85f7d09u,0xc80a7d09u}) {
    const auto original=Bytes(word),ordered=Bytes(word|0x8000);
    check(OriginalExclusive(word));
    check(MatchOriginalGuard(original,original,0x1000,GuardDomain::A64Code));
    check(!MatchOriginalGuard(ordered,original,0x1000,GuardDomain::A64Code));
    check(MatchOriginalGuard(ordered,original,0x1000,GuardDomain::A64Code,true));
    check(!MatchOriginalGuard(ordered,original,0x1000,GuardDomain::Data,true));
    check(!MatchOriginalGuard(original,ordered,0x1000,GuardDomain::A64Code,true));
    check(!MatchOriginalGuard(ordered,original,0x1001,GuardDomain::A64Code,true));
    for(unsigned bit=0;bit<32;++bit) {
      if(bit==15)continue;
      check(!MatchOriginalGuard(Bytes((word|0x8000)^(1u<<bit)),original,
                                0x1000,GuardDomain::A64Code,true));
    }
    auto big=ordered;std::reverse(big.begin(),big.end());
    check(!MatchOriginalGuard(big,original,0x1000,GuardDomain::A64Code,true));
  }
  for(uint32_t word:{0xd503201fu,0x94000000u,0xd65f03c0u,0x08400000u}) {
    check(!OriginalExclusive(word));
    check(!MatchOriginalGuard(Bytes(word|0x8000),Bytes(word),0,
                              GuardDomain::A64Code,true));
  }
  const auto ordered=Bytes(0xc85ffc00u);
  check(MatchOriginalGuard(ordered,ordered,0,GuardDomain::A64Code,true));
  check(!MatchOriginalGuard({}, {},0,GuardDomain::Data,true));
  check(!MatchOriginalGuard(std::span(ordered).first(3),ordered,0,GuardDomain::A64Code,true));
  check(!MatchOriginalGuard(std::span(ordered).first(3),std::span(ordered).first(3),0,GuardDomain::A64Code,true));
  check(MatchOriginalGuard(std::span(ordered).first(3),std::span(ordered).first(3),1,GuardDomain::Data,true));
  std::vector<uint8_t> original(4096),actual(4096);
  for(size_t i=0;i<4096;i+=4) {
    const auto from=Bytes(i%8?0xd503201fu:0xc85f7c00u);
    const auto to=Bytes(i%8?0xd503201fu:0xc85ffc00u);
    std::copy(from.begin(),from.end(),original.begin()+i);
    std::copy(to.begin(),to.end(),actual.begin()+i);
  }
  check(MatchOriginalGuard(actual,original,0,GuardDomain::A64Code,true));
  actual[4095]^=1;
  check(!MatchOriginalGuard(actual,original,0,GuardDomain::A64Code,true));
  original.push_back(0);actual.push_back(0);
  check(!MatchOriginalGuard(actual,original,0,GuardDomain::Data,true));
  check(MatchSupportedGuard(Bytes(0xc85ffc00u),Bytes(0xc85f7c00u),0x21cdae0));
  check(!MatchSupportedGuard(Bytes(0xc85ffc00u),Bytes(0xc85f7c00u),0x28daad4));
  check(MatchSupportedGuard(Bytes(0xc85f7c00u),Bytes(0xc85f7c00u),0x28daad4));
  std::cout<<checks<<" bounded code guard checks passed\n";
}
