// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"
#include <algorithm>
#include <array>
#include <limits>
namespace echoes {
// A native mode1 list stores actual/target as one aligned pair. Native filter
// rebuild also assigns both together (26b3480/3484). Only the settled held UI
// cursor changes here: native DRight release still owns the equip operation.
// Caller must freshly revalidate the complete expected hold immediately before
// invoking this function, and observe native readback afterwards. No retries.
template<class Write> bool DirectSelect(const LiveSnapshot& value,uint16_t id,const ReadFn& read,Write&& write) {
  const auto& s=value.selector;
  if(!value.compatible || !value.collection_valid || !s.compatible ||
     !s.selector_valid || !s.held || s.source.mode!=1 || s.source.native_state!=2 ||
     !s.source.owner || s.source.owner%8 ||
     s.source.owner>std::numeric_limits<uint64_t>::max()-0x2df0 ||
     s.ordered_native_ids.empty() || s.ordered_native_ids.size()>512 ||
     s.highlighted_index<0 || size_t(s.highlighted_index)>=s.ordered_native_ids.size() ||
     s.target_index!=s.highlighted_index ||
     s.highlighted_native_id!=s.ordered_native_ids[size_t(s.highlighted_index)] ||
     !s.source.records || s.source.records%8 || s.source.end<s.source.records ||
     s.source.end-s.source.records!=s.ordered_native_ids.size()*0x38 ||
     s.source.capacity<s.source.end ||
     std::find(value.learned_native_ids.begin(),value.learned_native_ids.end(),id)==value.learned_native_ids.end())return false;
  const auto at=std::find(s.ordered_native_ids.begin(),s.ordered_native_ids.end(),id);
  if(at==s.ordered_native_ids.end())return false;
  // Queue the native Change state only from its settled Wait state. Its
  // native entry callback (26b21c4 -> vtable+d0 -> 26b4160) repopulates the
  // carousel through vtable+b0/26b3e98, so art and name agree on the same jump.
  std::array<uint8_t,3> state{};std::array<uint8_t,4> direction{};
  const uint64_t list=s.source.owner+0x150;
  if(!read || !read(list+0x98,state.data(),state.size()) || state[0]!=1 || state[2]!=0xfe ||
     !read(list+0x2c98,direction.data(),direction.size()) || direction[0]>3 ||
     direction[1] || direction[2] || direction[3])return false;
  const uint32_t index=uint32_t(at-s.ordered_native_ids.begin());
  if(index==uint32_t(s.highlighted_index))return true;
  std::array<uint8_t,8> pair{};
  for(size_t i=0;i<4;++i)pair[i]=pair[i+4]=uint8_t(index>>(i*8));
  if(!write(list+0x2c78,pair.data(),pair.size()))return false;
  // Native filter rebuild uses direction2 for a same-index repaint. Do not
  // toggle favorite/filter state or touch the equipped/save identity.
  const std::array<uint8_t,4> repaint{2,0,0,0};
  if(!write(list+0x2c98,repaint.data(),repaint.size()))return false;
  const uint8_t change=2;
  return write(list+0x9a,&change,1);
}
} // namespace echoes
