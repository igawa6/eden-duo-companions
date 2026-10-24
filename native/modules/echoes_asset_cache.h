// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <mutex>
namespace echoes {
// read_romfs archive-member range requests re-extract the complete member in
// runtime22. Field's texture index makes hundreds of small reads. Keep this
// single immutable, game-owned member for the module lifetime (at most32MiB).
class FieldArchiveCache {
 public:
  size_t Read(const ReadAssetFn& read,std::string_view path,uint64_t offset,void* out,size_t size) {
    constexpr std::string_view Key="region_common/ui/Field.arc#timg/__Combined.bntx";
    if(path!=Key)return read?read(path,offset,out,size):0;
    std::lock_guard lock(mutex_);
    if(bytes_.empty()) {
      const auto length=read?read(path,0,nullptr,0):0;
      if(!length || length>(32u<<20))return 0;
      std::vector<uint8_t> candidate(length);
      if(read(path,0,candidate.data(),candidate.size())!=length)return 0;
      bytes_=std::move(candidate);
    }
    if(!out)return bytes_.size();
    if(offset>=bytes_.size())return 0;
    const auto count=std::min(size,bytes_.size()-static_cast<size_t>(offset));
    std::memcpy(out,bytes_.data()+offset,count);return count;
  }
 private:
  std::mutex mutex_;std::vector<uint8_t> bytes_;
};
}
