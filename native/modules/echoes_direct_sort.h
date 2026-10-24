// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"
#include <algorithm>
#include <array>
#include <limits>
namespace echoes {
enum class DirectSortResult { Refused, Current, Prepared };
namespace direct_sort_detail {
inline uint64_t Le64(const uint8_t* p) {
  uint64_t n{};for(unsigned i=0;i<8;++i)n|=uint64_t(p[i])<<(i*8);return n;
}
struct Frame {
  std::array<uint8_t,3> state{};
  std::array<uint8_t,8> cursors{};
  std::array<uint8_t,40> header{};
  std::array<uint64_t,5> blocks{};
  std::array<uint8_t,5> values{};
  uint8_t index{},count{};
  bool operator==(const Frame&) const = default;
};
inline bool Read(const ReadFn& read,uint64_t owner,Frame& f) {
  const uint64_t list=owner+0x150;
  if(!read(list+0x98,f.state.data(),f.state.size()) || f.state[0]!=1 || f.state[2]!=0xfe ||
     !read(list+0x2c78,f.cursors.data(),f.cursors.size()) ||
     !read(owner+0x2dec,&f.index,1) ||
     !read(owner+0x2df8,f.header.data(),f.header.size()))return false;
  const auto map=Le64(f.header.data()),end=Le64(f.header.data()+8),
             start=Le64(f.header.data()+24),count=Le64(f.header.data()+32);
  if(!map || map%8 || end<=map || end%8 || end-map>64 ||
     !count || count>5 || f.index>=count || start>UINT64_MAX-count ||
     start+count>((end-map)/8)*4096)return false;
  f.count=uint8_t(count);
  uint8_t seen{};
  for(uint64_t i=0;i<count;++i) {
    const auto pos=start+i,slot=map+(pos/4096)*8;
    std::array<uint8_t,8> bytes{};
    if(!read(slot,bytes.data(),bytes.size()))return false;
    const auto block=Le64(bytes.data());f.blocks[size_t(i)]=block;
    if(!block || block%8 || block>UINT64_MAX-(pos%4096) ||
       !read(block+(pos%4096),&f.values[size_t(i)],1))return false;
    const auto v=f.values[size_t(i)];
    // The sixth native case has no supported label. Never select through it.
    if(!(v==1 || v==2 || v==4 || v==8 || v==16) || (seen&v))return false;
    seen|=v;
  }
  return true;
}
} // namespace direct_sort_detail
// The native Y handler increments this INDEX once, then runs its original
// comparator and native carousel transition (26b3394..26b33dc). Prepositioning
// its predecessor therefore selects any available target in ONE native sort
// operation; it does not fake the label or rewrite the sorted record vector.
// Caller must freshly revalidate the complete hold immediately before calling,
// issue exactly one Y pulse after Prepared, stop it upon native acknowledgement,
// and await the requested enum plus settled native list. No retries/hops.
template<class Write> DirectSortResult PrepareDirectSort(const LiveSnapshot& value,
    uint8_t target,const ReadFn& read,Write&& write) {
  using namespace direct_sort_detail;
  const auto& s=value.selector;
  if(!read || !value.compatible || !value.collection_valid || !s.compatible ||
     !s.selector_valid || !s.held || s.source.mode!=1 || s.source.native_state!=2 ||
     !s.source.owner || s.source.owner%8 || s.source.owner>UINT64_MAX-0x2e20 ||
     !s.sort_valid || !s.source.sort_valid || s.sort_enum!=s.source.sort_enum ||
     s.ordered_native_ids.empty() || s.ordered_native_ids.size()>512 ||
     s.highlighted_index<0 || size_t(s.highlighted_index)>=s.ordered_native_ids.size() ||
     s.target_index!=s.highlighted_index ||
     s.highlighted_native_id!=s.ordered_native_ids[size_t(s.highlighted_index)] ||
     !s.source.records || s.source.records%8 || s.source.end<s.source.records ||
     s.source.end-s.source.records!=s.ordered_native_ids.size()*0x38 ||
     s.source.capacity<s.source.end)return DirectSortResult::Refused;
  Frame a,b;
  if(!Read(read,s.source.owner,a) || !Read(read,s.source.owner,b) || a!=b ||
     Le64(a.cursors.data())!=(uint64_t(uint32_t(s.highlighted_index)) |
                               uint64_t(uint32_t(s.target_index))<<32) ||
     a.values[a.index]!=s.sort_enum)return DirectSortResult::Refused;
  const auto at=std::find(a.values.begin(),a.values.begin()+a.count,target);
  if(at==a.values.begin()+a.count)return DirectSortResult::Refused;
  if(target==s.sort_enum)return DirectSortResult::Current;
  const auto index=uint8_t(at-a.values.begin());
  const uint8_t predecessor=uint8_t((index+a.count-1)%a.count);
  if(predecessor!=a.index && !write(s.source.owner+0x2dec,&predecessor,1))
    return DirectSortResult::Refused;
  return DirectSortResult::Prepared;
}
} // namespace echoes
